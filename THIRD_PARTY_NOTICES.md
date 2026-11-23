# Third-party notices

SMLoader itself is MIT licensed (see [LICENSE](LICENSE)). `Release\winmm.dll`
also contains the following third-party code, in `loader/fixes/`.

## FastPhysics 1.1.0 (zlib licence)

The fast constraint solver (`fastlcp.cpp`, `fastlcp.h`, `lcpkernels.h`) and the
way it is hooked come from FastPhysics 1.1.0, a Scrap Mechanic mod:
https://www.nexusmods.com/scrapmechanic/mods/63

SMLoader contains an **altered version**: the solver's statistics printout was
removed and one thread-local variable moved, the hook around it was rewritten,
and its two other patches are not included. Problems with it are SMLoader's, not
the original author's.

```
This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the
use of this software. Permission is granted to anyone to use this software for
any purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim
   that you wrote the original software. If you use this software in a product,
   an acknowledgment in the product documentation would be appreciated but is
   not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## Bullet Physics (zlib licence)

`fastlcp.cpp` is derived from Bullet's `btDantzigLCP.cpp`.
Bullet Continuous Collision Detection and Physics Library,
http://bulletphysics.org. Same licence text as above.

## Open Dynamics Engine (BSD-style licence)

Bullet's solver is a port of the Open Dynamics Engine's.

```
This is the BSD-style license for the Open Dynamics Engine
----------------------------------------------------------

Open Dynamics Engine
Copyright (c) 2001-2007, Russell L. Smith.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

Neither the names of ODE's copyright owner nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

# Credits

- **networking-fix** by Scrap-Mods (https://github.com/Scrap-Mods/networking-fix)
  found that the game's client stalls packets and that not postponing them fixes
  it. SMLoader's network fix is a separate implementation and contains none of
  its code.
