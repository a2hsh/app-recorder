@echo off
REM ===========================================================================
REM apprecorder-wait.cmd -- run apprecorder and actually WAIT for it.
REM
REM WHAT THIS IS FOR
REM
REM   apprecorder.exe is one executable with two front ends: a window when you
REM   double-click it, a command line when you give it a command. A PE's
REM   subsystem is fixed in its header and there is only one header, so the
REM   image is marked WINDOWS -- otherwise every double-click would flash a
REM   black console.
REM
REM   cmd.exe reads that same flag to decide whether to wait for a child. It
REM   therefore does NOT wait for apprecorder.exe: the prompt comes back
REM   immediately, output interleaves with whatever you type next, and
REM
REM       apprecorder record --exe teams.exe --out mix.wav && upload.ps1
REM
REM   stops sequencing -- upload.ps1 runs while the recording is still going,
REM   against a file that is not finished.
REM
REM   This shim restores that. It waits, and it hands back apprecorder's own
REM   exit code, so every %ERRORLEVEL% test and every && / || in a script means
REM   what it meant before:
REM
REM       apprecorder-wait record --exe teams.exe --out mix.wav && upload.ps1
REM
REM   The exit codes are contract and are listed by `apprecorder help`:
REM     0 done  1 usage  2 config  3 not found  4 output  5 capture
REM     6 incomplete  7 internal
REM
REM WHAT IT DOES NOT DO: REDIRECTION
REM
REM   `start` does not hand its own standard handles to the process it
REM   launches -- measured, not assumed -- so
REM
REM       apprecorder-wait record ... > log.txt
REM
REM   leaves log.txt empty. The output is not lost: apprecorder attaches to the
REM   console it was launched from, so at a prompt you see everything as usual.
REM   It just does not follow a > into a file.
REM
REM   If you want the output in a file, redirect the EXECUTABLE, which binds
REM   the handle before the process starts and is left alone:
REM
REM       apprecorder record ... > log.txt      (output kept, no sequencing)
REM       apprecorder-wait record ... && next   (sequencing kept, output to the console)
REM
REM   Or ask for a log by name, which is unaffected by either:
REM
REM       apprecorder-wait record ... --log-file run.log --log-level info
REM
REM WHY THE NAME IS NOT apprecorder.cmd
REM
REM   PATHEXT is searched in order and .EXE comes before .CMD, so a file called
REM   apprecorder.cmd sitting beside apprecorder.exe would never be found by
REM   typing "apprecorder" -- the shim would look installed and do nothing. The
REM   different name is the point, not an accident.
REM
REM   It finds the executable through %~dp0, so keep the two together.
REM
REM WHY NOT A SEPARATE CONSOLE BUILD INSTEAD
REM
REM   That is the 1.75 MB of near-identical bytes this merge exists to remove.
REM   See include/frontend.h.
REM ===========================================================================

if not exist "%~dp0apprecorder.exe" (
  echo apprecorder-wait: apprecorder.exe is not next to this script ^(%~dp0^). 1>&2
  exit /b 7
)

REM start /wait is what makes cmd.exe wait for a WINDOWS-subsystem image. The
REM empty "" is the window title start/ would otherwise take the quoted program
REM path for; %* passes the command line through untouched.
start /wait "" "%~dp0apprecorder.exe" %*
exit /b %errorlevel%
