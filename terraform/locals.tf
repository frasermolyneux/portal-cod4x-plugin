locals {
  location_short_map = {
    swedencentral = "swc"
    uksouth       = "uks"
    ukwest        = "ukw"
  }

  location_short = lookup(local.location_short_map, lower(var.location), "swc")

  workload_resource_group = data.terraform_remote_state.platform_workloads.outputs.workload_resource_groups[var.workload_name][var.environment].resource_groups[lower(var.location)]

  workload_service_principal = data.terraform_remote_state.platform_workloads.outputs.workload_service_principals[var.workload_name][var.environment]
}
