@echo off
setlocal
pushd "%~dp0..\..\..\..\.."
if not exist Build\EmmcDumpHostChecks mkdir Build\EmmcDumpHostChecks
pushd Build\EmmcDumpHostChecks
cl /nologo /Gy /DMDEPKG_NDEBUG /I..\..\MdePkg\Include /I..\..\MdePkg\Include\Ia32 ^
  ..\..\QcomPkg\Msm8960Pkg\Applications\EmmcDump\tests\HostChecks.c ^
  ..\..\MdePkg\Library\BasePrintLib\PrintLib.c ^
  ..\..\MdePkg\Library\BasePrintLib\PrintLibInternal.c ^
  /Fe:HostChecks.exe /link /OPT:REF
if errorlevel 1 goto failed
HostChecks.exe
if errorlevel 1 goto failed
popd
popd
exit /b 0
:failed
popd
popd
exit /b 1
