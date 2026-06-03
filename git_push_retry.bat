@echo off
setlocal

set REMOTE=origin
set BRANCH=main
set DELAY=10
set ATTEMPT=0

echo [%date% %time%] Starting git push retry loop...
echo Remote: %REMOTE% / Branch: %BRANCH%
echo Retrying every %DELAY% seconds on failure.
echo.

:retry
set /a ATTEMPT+=1
echo [%date% %time%] Attempt #%ATTEMPT%...

git push %REMOTE% %BRANCH%

if %ERRORLEVEL%==0 (
    echo.
    echo [%date% %time%] SUCCESS on attempt #%ATTEMPT%!
    goto done
)

echo [%date% %time%] Failed. Retrying in %DELAY% seconds...
timeout /t %DELAY% /nobreak >nul
goto retry

:done
echo.
echo Push complete. You can close this window.
pause
