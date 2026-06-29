workload_name = "portal-cod4x-plugin"
environment   = "dev"
location      = "swedencentral"

subscription_id = "6cad03c1-9e98-4160-8ebe-64dd30f1bbc7"

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
  Environment = "dev"
  Workload    = "portal-cod4x-plugin"
  DeployedBy  = "GitHub-Terraform"
  Git         = "https://github.com/frasermolyneux/portal-cod4x-plugin"
  Owner       = "globaladdy@molyneux.io"
  Source      = "https://github.com/frasermolyneux/portal-cod4x-plugin"
}
