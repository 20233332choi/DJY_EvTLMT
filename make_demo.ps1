$ErrorActionPreference = "Stop"
Set-Location -LiteralPath $PSScriptRoot
python .\generate_ev_log.py --demo --output .\output\formula_e_demo.log
Write-Host "Created output\formula_e_demo.log"
