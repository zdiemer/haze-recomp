# Profile-guided optimization

The lifted PPU modules are compiled with a committed Clang instrumentation
profile, `port/pgo/lifted.profdata.tar.xz`. The game's main thread runs
megabytes of lifted code per frame with no dominant function, so it is limited
by instruction fetch rather than by any one hot loop. The profile lets the
compiler lay out and inline along the paths gameplay actually takes. In the
later-combat camera sweep it cut frame time by about 11% (15.2 to 13.6 ms
mean over five alternating pairs).

Every Clang build applies the profile; there is no switch. It contains function
names, block counts and control-flow hashes of the lifted code, not code or
game data.

The profile is sparse: functions it never recorded keep ordinary optimization
rather than being treated as cold. If a lifter or patch change alters a lifted
function, Clang ignores that function's stale entry, so the build still works
and the function loses only its layout benefit. Record a new profile after
large lifter or patch-chain changes.

## Recording a new profile

1. Build the instrumented game into its own directory:

   ```powershell
   scripts/build.ps1 -SharedRoot <checkout> -Configuration Release -PgoGenerate -OutputDirectory local/bin-pgogen
   ```

2. Play each scene to cover, closing the game normally from its window so
   the profile is written at exit. Name one file per session:

   ```powershell
   $env:LLVM_PROFILE_FILE = "$PWD\local\pgo\%p.profraw"
   local\bin-pgogen\HazeRecomp.exe
   ```

   The instrumented game runs roughly a third slower. The current profile
   covers the later-combat checkpoint and the first jungle checkpoint.

3. Merge the sessions sparsely and pack the result:

   ```powershell
   $llvm = "<Visual Studio>\VC\Tools\Llvm\x64\bin"
   & "$llvm\llvm-profdata.exe" merge --sparse -o local\pgo\lifted.profdata local\pgo\*.profraw
   tar -C local\pgo -cJf port\pgo\lifted.profdata.tar.xz lifted.profdata
   ```

4. Rebuild normally, then compare the new build against the previous one in
   gameplay before committing the archive.
