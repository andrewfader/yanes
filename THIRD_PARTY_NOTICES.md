# Third-party notices

## ymfm

YANES uses Aaron Giles' `ymfm` Yamaha FM sound cores, pinned to commit
`81aec25ccbb98f4873a255f7551ac4dadac59b4a`, with one modification applied at fetch time in
CMakeLists.txt: ymfm_fm.ipp's OPL/OPLL hi-hat and top-cymbal phase select is corrected to
match the die-derived Nuked-OPLL/Nuked-OPL3 (see FURNACE_PARITY.md).

Copyright (c) 2021, Aaron Giles. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted
provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions
   and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of
   conditions and the following disclaimer in the documentation and/or other materials provided
   with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors may be used to
   endorse or promote products derived from this software without specific prior written
   permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## CLAP headers

The build fetches the CLAP plug-in API headers (https://github.com/free-audio/clap, tag 1.2.7),
or uses the checkout named by `-DCLAP_ROOT`. They are used under the MIT license:

Copyright (c) 2021 Alexandre BIQUE. Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights to use, copy,
modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

## Test-only references (not part of the plug-in)

The hardware-oracle test lanes run external emulators and reference cores that are never
compiled into or distributed with `YANES.clap`. The small instrumentation patches under
`third_party/` apply to those projects' own checkouts and are released under each project's
license:

- `sameboy-apu-register-log.patch`: SameBoy (MIT, Lior Halphon).
- `nestopia-apu-register-log.patch`: Nestopia UE (GPL-2.0).
- `beetle-pce-psg-register-log.patch`: Beetle PCE / Mednafen (GPL-2.0).
- `genesis-plus-gx-psg-register-log.patch`: Genesis Plus GX (non-commercial license, see its
  LICENSE.txt).

The optional parity tests also build against blargg's `SPC_DSP` (LGPL-2.1, from a Furnace
checkout), reSIDfp (GPL-2.0, system `libresidfp`), and render reference audio with Furnace
(GPL-2.0) and REAPER. Each is linked into or run by a test executable only.
