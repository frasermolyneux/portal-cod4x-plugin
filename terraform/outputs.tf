output "artifacts_storage_account_name" {
  description = "Storage account name hosting portal-cod4x-plugin build artifacts"
  value       = azurerm_storage_account.artifacts.name
}

output "artifacts_storage_account_id" {
  description = "Storage account resource ID hosting portal-cod4x-plugin build artifacts"
  value       = azurerm_storage_account.artifacts.id
}

output "artifacts_storage_container_name" {
  description = "Blob container name used for portal-cod4x-plugin build artifacts"
  value       = azurerm_storage_container.artifacts.name
}

output "artifacts_resource_group_name" {
  description = "Resource group name containing the artifacts storage account"
  value       = data.azurerm_resource_group.workload.name
}
