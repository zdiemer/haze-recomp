# Profile-guided optimization

The lifted code is compiled with two committed Clang instrumentation profiles:

- `port/pgo/lifted.profdata.tar.xz` for the lifted PPU modules. The game's
  main thread runs megabytes of lifted code per frame with no dominant
  function, so it is limited by instruction fetch rather than by any one hot
  loop. The profile lets the compiler lay out and inline along the paths
  gameplay actually takes. In the later-combat camera sweep it cut frame time
  by about 11% (15.2 to 13.6 ms mean over five alternating pairs).
- `port/pgo/spu.profdata.tar.xz` for the lifted SPU images. The main thread
  waits on several of their jobs each frame. In later combat it cut the
  image-32 effects job's busy time by about 6% and the main thread's wait for
  it by about 0.23 ms per frame (three alternating job traces each), and the
  offline image-32 replay by about 4%.

Each side uses only its own profile, so recording or changing one never moves
the other's hot/cold thresholds. Every Clang build applies both; there is no
switch. They contain function names, block counts and control-flow hashes of
the lifted code, not code or game data.

The profiles are sparse: functions they never recorded keep ordinary
optimization rather than being treated as cold. If a lifter or patch change
alters a lifted function, Clang ignores that function's stale entry, so the
build still works and the function loses only its layout benefit. Record new
profiles after large lifter or patch-chain changes.

## Recording new profiles

1. Build the instrumented game into its own directory. It instruments the
   lifted PPU modules and SPU images together:

   ```powershell
   scripts/build.ps1 -SharedRoot <checkout> -Configuration Release -PgoGenerate -OutputDirectory local/bin-pgogen
   ```

2. Play each scene to cover, closing the game normally from its window so
   the profile is written at exit. Name one file per session:

   ```powershell
   $env:LLVM_PROFILE_FILE = "$PWD\local\pgo\%p.profraw"
   local\bin-pgogen\HazeRecomp.exe
   ```

   The instrumented game runs roughly a third slower. The PPU profile covers
   the later-combat checkpoint and the first jungle checkpoint; the SPU
   profile covers later combat.

3. Merge the sessions sparsely, split them into the two profiles and pack
   them. PPU functions are C++ names (MSVC-mangled, a `.cpp` file prefix on
   static functions, or the `ppu_` HLE registration); SPU images are C:

   ```powershell
   $llvm = "<Visual Studio>\VC\Tools\Llvm\x64\bin"
   $ppu = '\?|\.cpp[:;]|^ppu_'
   & "$llvm\llvm-profdata.exe" merge --sparse -o local\pgo\all.profdata local\pgo\*.profraw
   & "$llvm\llvm-profdata.exe" merge --sparse --function=$ppu -o local\pgo\lifted.profdata local\pgo\all.profdata
   & "$llvm\llvm-profdata.exe" merge --sparse --no-function=$ppu -o local\pgo\spu.profdata local\pgo\all.profdata
   tar -C local\pgo -cJf port\pgo\lifted.profdata.tar.xz lifted.profdata
   tar -C local\pgo -cJf port\pgo\spu.profdata.tar.xz spu.profdata
   ```

   `llvm-profdata show` should report the same function total for
   `all.profdata` as for the two halves combined.

4. Rebuild normally, then compare the new build against the previous one in
   gameplay before committing the archives.
