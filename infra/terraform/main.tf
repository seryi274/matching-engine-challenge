# Matching Engine Challenge -- AWS Infrastructure
#
# Usage:
#   1. Copy terraform.tfvars.example to terraform.tfvars and fill in your values.
#   2. terraform init        -- download providers and initialize the backend
#   3. terraform plan        -- preview what will be created
#   4. terraform apply       -- provision the infrastructure
#   5. terraform destroy     -- tear down everything when you are done
#
# All secrets (webhook_secret, github_token) are passed as variables and never
# written to disk. Mark them sensitive in variables.tf so Terraform redacts them
# from CLI output.

terraform {
  required_version = ">= 1.5.0"

  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.40"
    }
  }
}

# ---------------------------------------------------------------------------
# Provider
# ---------------------------------------------------------------------------

provider "aws" {
  region = var.aws_region

  default_tags {
    tags = {
      Project   = "matching-engine-challenge"
      ManagedBy = "terraform"
    }
  }
}

# ---------------------------------------------------------------------------
# Data sources
# ---------------------------------------------------------------------------

# Latest Ubuntu 24.04 LTS (Noble Numbat) AMI from Canonical.
data "aws_ami" "ubuntu" {
  most_recent = true
  owners      = ["099720109477"] # Canonical

  filter {
    name   = "name"
    values = ["ubuntu/images/hvm-ssd-gp3/ubuntu-noble-24.04-amd64-server-*"]
  }

  filter {
    name   = "virtualization-type"
    values = ["hvm"]
  }

  filter {
    name   = "architecture"
    values = ["x86_64"]
  }

  filter {
    name   = "state"
    values = ["available"]
  }
}

# Fetch the default VPC so we can use it when var.use_default_vpc is true.
data "aws_vpc" "default" {
  count   = var.use_default_vpc ? 1 : 0
  default = true
}

# Pick the first available subnet in the default VPC.
data "aws_subnets" "default" {
  count = var.use_default_vpc ? 1 : 0

  filter {
    name   = "vpc-id"
    values = [data.aws_vpc.default[0].id]
  }

  filter {
    name   = "default-for-az"
    values = ["true"]
  }
}

# ---------------------------------------------------------------------------
# Networking -- custom VPC (created only when use_default_vpc = false)
# ---------------------------------------------------------------------------

resource "aws_vpc" "this" {
  count                = var.use_default_vpc ? 0 : 1
  cidr_block           = "10.0.0.0/16"
  enable_dns_support   = true
  enable_dns_hostnames = true

  tags = { Name = "matching-engine-challenge-vpc" }
}

resource "aws_internet_gateway" "this" {
  count  = var.use_default_vpc ? 0 : 1
  vpc_id = aws_vpc.this[0].id

  tags = { Name = "matching-engine-challenge-igw" }
}

resource "aws_subnet" "public" {
  count                   = var.use_default_vpc ? 0 : 1
  vpc_id                  = aws_vpc.this[0].id
  cidr_block              = "10.0.1.0/24"
  map_public_ip_on_launch = true
  availability_zone       = "${var.aws_region}a"

  tags = { Name = "matching-engine-challenge-public" }
}

resource "aws_route_table" "public" {
  count  = var.use_default_vpc ? 0 : 1
  vpc_id = aws_vpc.this[0].id

  route {
    cidr_block = "0.0.0.0/0"
    gateway_id = aws_internet_gateway.this[0].id
  }

  tags = { Name = "matching-engine-challenge-rt" }
}

resource "aws_route_table_association" "public" {
  count          = var.use_default_vpc ? 0 : 1
  subnet_id      = aws_subnet.public[0].id
  route_table_id = aws_route_table.public[0].id
}

# ---------------------------------------------------------------------------
# Locals -- resolve VPC / subnet IDs regardless of mode
# ---------------------------------------------------------------------------

locals {
  vpc_id    = var.use_default_vpc ? data.aws_vpc.default[0].id : aws_vpc.this[0].id
  subnet_id = var.use_default_vpc ? data.aws_subnets.default[0].ids[0] : aws_subnet.public[0].id
}

# ---------------------------------------------------------------------------
# Security group
# ---------------------------------------------------------------------------

resource "aws_security_group" "instance" {
  name_prefix = "matching-engine-challenge-"
  description = "Allow SSH and leaderboard HTTP access"
  vpc_id      = local.vpc_id

  # SSH from the allowed CIDR
  ingress {
    description = "SSH"
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = [var.ssh_allowed_cidr]
  }

  # Leaderboard / webhook HTTP
  ingress {
    description = "HTTP leaderboard"
    from_port   = 8000
    to_port     = 8000
    protocol    = "tcp"
    cidr_blocks = ["0.0.0.0/0"]
  }

  # All outbound
  egress {
    description = "All outbound"
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  lifecycle {
    create_before_destroy = true
  }

  tags = { Name = "matching-engine-challenge-sg" }
}

# ---------------------------------------------------------------------------
# EC2 instance
# ---------------------------------------------------------------------------

resource "aws_instance" "challenge" {
  ami                    = data.aws_ami.ubuntu.id
  instance_type          = var.instance_type
  key_name               = var.key_pair_name
  subnet_id              = local.subnet_id
  vpc_security_group_ids = [aws_security_group.instance.id]
  tenancy                = var.dedicated_tenancy ? "dedicated" : "default"

  root_block_device {
    volume_size           = 30
    volume_type           = "gp3"
    delete_on_termination = true
    encrypted             = true
  }

  user_data = base64encode(templatefile("${path.module}/user-data.sh.tftpl", {
    repo_url       = var.repo_url
    repo_branch    = var.repo_branch
    webhook_secret = var.webhook_secret
    github_token   = var.github_token
  }))

  user_data_replace_on_change = true

  metadata_options {
    http_endpoint               = "enabled"
    http_tokens                 = "required" # IMDSv2
    http_put_response_hop_limit = 1
  }

  tags = {
    Name    = "matching-engine-challenge"
    Project = "matching-engine-challenge"
  }
}

# ---------------------------------------------------------------------------
# Elastic IP -- survives stop/start cycles
# ---------------------------------------------------------------------------

resource "aws_eip" "challenge" {
  domain = "vpc"

  tags = { Name = "matching-engine-challenge-eip" }
}

resource "aws_eip_association" "challenge" {
  instance_id   = aws_instance.challenge.id
  allocation_id = aws_eip.challenge.id
}
