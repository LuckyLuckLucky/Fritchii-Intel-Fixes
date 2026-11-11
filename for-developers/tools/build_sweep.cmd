@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\Git\cmd;C:\Program Files\Git\usr\bin;%VULKAN_SDK%\Bin;%PATH%"
cmake -S C:\work\Fritchii-intel\Fritchii-src -B C:\work\Fritchii-intel\_build -DENABLE_DEBUG_TOOLS=ON || exit /b 1
cmake --build C:\work\Fritchii-intel\_build --target shader-sweep --parallel 12

