@echo off
if not exist "luth\assets\shaders\spv" mkdir "luth\assets\shaders\spv"

echo Compiling Shaders...

%VULKAN_SDK%\Bin\glslc.exe luth\assets\shaders\triangle.vert -o luth\assets\shaders\spv\triangle.vert.spv
%VULKAN_SDK%\Bin\glslc.exe luth\assets\shaders\triangle.frag -o luth\assets\shaders\spv\triangle.frag.spv

echo Done.
pause
