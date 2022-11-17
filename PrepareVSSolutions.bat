@PUSHD %~dp0
mkdir build
cd build
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvarsall.bat" x64 10.0.19041.0
set SYNERGY_ENTERPRISE=TRUE
cmake -G "Visual Studio 16 2019" -DCMAKE_BUILD_TYPE=Debug ..
REM msbuild synergy-core.sln /p:Platform="x64" /p:Configuration=Debug /m
cd ..
copy ext\openssl\windows\x64\bin\* build\
@POPD

