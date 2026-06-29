resource "azurerm_storage_account" "artifacts" {
  name = "stor${substr(md5(var.workload_name), 0, 4)}${var.environment}${local.location_short}${random_id.environment_id.hex}"

  resource_group_name = data.azurerm_resource_group.workload.name
  location            = data.azurerm_resource_group.workload.location

  account_tier             = "Standard"
  account_replication_type = "LRS"
  account_kind             = "StorageV2"
  access_tier              = "Hot"

  https_traffic_only_enabled      = true
  min_tls_version                 = "TLS1_2"
  allow_nested_items_to_be_public = false

  local_user_enabled        = false
  shared_access_key_enabled = false

  tags = var.tags
}

resource "azurerm_storage_container" "artifacts" {
  name = var.artifacts_container_name

  storage_account_id    = azurerm_storage_account.artifacts.id
  container_access_type = "private"
}

resource "azurerm_role_assignment" "workload_blob_data_contributor" {
  scope                = azurerm_storage_account.artifacts.id
  role_definition_name = "Storage Blob Data Contributor"
  principal_id         = local.workload_service_principal.service_principal_object_id
}
