# WAV decoder host check

This MinGW test compiles the production `dec_wav.c` with a FAT32-sized file
API shim. Each test case supplies a WAV path, expected duration and audio
format. The test reads the start, middle and end of each file, checks seeking
and verifies clean EOF. Set the last argument to `1` for files whose audio
data extends beyond 2 GB.

From the project root in PowerShell:

```powershell
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -municode -Itests/wav_host_shim -Icomponents/music/include -Icomponents/music/src tests/wav_host_test.c components/music/src/dec_wav.c -o build-s1/wav_host_test.exe
& .\build-s1\wav_host_test.exe '<path-to-wav>' <duration_ms> <sample_rate> <bits> <channels> <check_seek_past_2gb:0-or-1>
```

Repeat the six arguments to test multiple files. For a truncated WAV, use the
duration of complete audio frames physically present in the file.
