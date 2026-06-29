workload_name = "portal-cod4x-plugin"
environment   = "prd"
location      = "uksouth"

subscription_id = "32444f38-32f4-409f-889c-8e8aa2b5b4d1"

platform_workloads_state = {
  resource_group_name  = "rg-tf-platform-workloads-prd-uksouth-01"
  storage_account_name = "sadz9ita659lj9xb3"
  container_name       = "tfstate"
  key                  = "terraform.tfstate"
  subscription_id      = "7760848c-794d-4a19-8cb2-52f71a21ac2b"
  tenant_id            = "e56a6947-bb9a-4a6e-846a-1f118d1c3a14"
}

artifacts_container_name = "plugin-artifacts"

tags = {
  Environment = "prd"
  Workload    = "portal-cod4x-plugin"
  DeployedBy  = "GitHub-Terraform"
  Git         = "https://github.com/frasermolyneux/portal-cod4x-plugin"
  Owner       = "globaladdy@molyneux.io"
  Source      = "https://github.com/frasermolyneux/portal-cod4x-plugin"
}
