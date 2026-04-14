# ---------------------------------------------------------------------------
# Variables -- Matching Engine Challenge infrastructure
# ---------------------------------------------------------------------------

variable "aws_region" {
  description = "AWS region to deploy into."
  type        = string
  default     = "eu-west-1"
}

variable "instance_type" {
  description = "EC2 instance type. Use a compute-optimized family (c5, c6i, c7i) for reliable benchmarks."
  type        = string
  default     = "c5.xlarge"
}

variable "key_pair_name" {
  description = "Name of an existing EC2 key pair for SSH access. Required."
  type        = string
}

variable "ssh_allowed_cidr" {
  description = "CIDR block allowed to SSH into the instance. Restrict this to your IP for security (e.g. 203.0.113.42/32)."
  type        = string
  default     = "0.0.0.0/0"
}

variable "repo_url" {
  description = "HTTPS URL of the matching-engine-challenge GitHub repository to clone."
  type        = string
}

variable "repo_branch" {
  description = "Git branch to check out after cloning."
  type        = string
  default     = "main"
}

variable "webhook_secret" {
  description = "Shared secret for validating GitHub webhook payloads. Keep this out of version control."
  type        = string
  sensitive   = true
}

variable "dedicated_tenancy" {
  description = "Run the instance on dedicated hardware for benchmark consistency. Costs significantly more."
  type        = bool
  default     = false
}

variable "github_token" {
  description = "GitHub personal access token for cloning private repositories. Leave empty for public repos."
  type        = string
  default     = ""
  sensitive   = true
}

variable "use_default_vpc" {
  description = "Use the account's default VPC instead of creating a new one. Simpler but less isolated."
  type        = bool
  default     = true
}
