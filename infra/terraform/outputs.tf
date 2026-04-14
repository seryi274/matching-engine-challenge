# ---------------------------------------------------------------------------
# Outputs -- Matching Engine Challenge infrastructure
# ---------------------------------------------------------------------------

output "instance_id" {
  description = "EC2 instance ID."
  value       = aws_instance.challenge.id
}

output "public_ip" {
  description = "Elastic IP attached to the instance (stable across stop/start)."
  value       = aws_eip.challenge.public_ip
}

output "ssh_command" {
  description = "SSH command to connect to the instance."
  value       = "ssh -i <path-to-${var.key_pair_name}.pem> ubuntu@${aws_eip.challenge.public_ip}"
}

output "leaderboard_url" {
  description = "URL of the leaderboard web interface."
  value       = "http://${aws_eip.challenge.public_ip}:8000"
}

output "webhook_url" {
  description = "URL to configure as the GitHub webhook endpoint."
  value       = "http://${aws_eip.challenge.public_ip}:8000/webhook"
}

output "status_api_url" {
  description = "Team status API endpoint (replace {team_name} with the actual team name)."
  value       = "http://${aws_eip.challenge.public_ip}:8000/api/status/{team_name}"
}

output "teardown_command" {
  description = "Run this to destroy all provisioned resources."
  value       = "cd ${abspath(path.module)} && terraform destroy"
}
