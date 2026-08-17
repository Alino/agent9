@echo off
setlocal
set IMAGE=agent9-v0.5.0.qcow2
set URL=https://github.com/Alino/agent9/releases/download/v0.5.0/%IMAGE%

if not exist "%IMAGE%" (
    echo Downloading %IMAGE%...
    powershell -Command "Invoke-WebRequest '%URL%' -OutFile '%IMAGE%'"
    echo Download complete.
)

rem Run VM (add qemu command here)
