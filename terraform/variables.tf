variable "workload_name" {
  description = "Name of the workload as defined in platform-workloads state"
  type        = string
  default     = "portal-cod4x-plugin"
}

variable "environment" {
  description = "Environment tag used for platform-workloads output indexing"
  type        = string
  default     = "dev"
}

variable "location" {
  description = "Azure region for the workload resource group"
  type        = string
  default     = "swedencentral"
}

variable "subscription_id" {
  description = "Azure subscription ID for this workload environment"
  type        = string
}

variable "platform_workloads_state" {
  description = "Backend config for platform-workloads remote state"
  type = object({
    resource_group_name  = string
    storage_account_name = string
    container_name       = string
    key                  = string
    subscription_id      = string
    tenant_id            = string
  })
}

variable "portal_environments_state" {
  description = "Backend config for portal-environments remote state"
  type = object({
    resource_group_name  = string
    storage_account_name = string
    container_name       = string
    key                  = string
    subscription_id      = string
    tenant_id            = string
  })
}

variable "artifacts_container_name" {
  description = "Blob container name used for plugin binary artifacts"
  type        = string
  default     = "plugin-artifacts"
}

variable "tags" {
  description = "Tags applied to all taggable resources"
  type        = map(string)
  default = {
    Environment = "dev"
    Workload    = "portal-cod4x-plugin"
    Owner       = "globaladdy@molyneux.io"
    Source      = "https://github.com/frasermolyneux/portal-cod4x-plugin"
  }
}
