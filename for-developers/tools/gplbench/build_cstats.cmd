@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d C:\work\Fritchii-intel\_tools\gplbench
cl /nologo /O2 /EHsc /std:c++20 /I"C:\VulkanSDK\1.4.363.0\Include" cstats.cpp /Fe:cstats.exe /link "C:\VulkanSDK\1.4.363.0\Lib\vulkan-1.lib"
