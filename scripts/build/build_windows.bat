@echo off

pushd ..\..
MSBuild Luth.sln /p:Configuration=Debug /p:Platform=x64
popd
pause
