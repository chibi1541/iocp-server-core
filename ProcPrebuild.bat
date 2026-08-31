pushd %~dp0

XCOPY "ServerCore/*.h" "../../Libraries/Include/ServerCore" /E /Y /I
XCOPY "ServerCore/*.hpp" "../../Libraries/Include/ServerCore" /E /Y /I

PAUSE

@ECHO OFF
REM Copy core headers to the location the server includes from.
REM
REM NOTE: keep this file ASCII only. cmd.exe reads .bat as the system codepage,
REM so UTF-8 comments turn into garbage and break parsing.

pushd "%~dp0"

SET "INCLUDE_DIR=..\..\Libraries\Include\ServerCore"

REM Clear the destination before copying.
REM XCOPY never removes stale files, so a deleted or renamed header leaves an
REM old copy behind and the client silently keeps compiling against it.
IF EXIST "%INCLUDE_DIR%" RMDIR /S /Q "%INCLUDE_DIR%"

XCOPY ServerCore\*.h "%INCLUDE_DIR%" /E /Y /I /Q
XCOPY ServerCore\*.hpp "%INCLUDE_DIR%" /E /Y /I /Q

popd