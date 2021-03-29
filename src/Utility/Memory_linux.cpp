/*
 * This file is part of QBDI.
 *
 * Copyright 2017 - 2021 Quarkslab
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "llvm/Support/Process.h"

#include "QBDI/Memory.h"
#include "QBDI/Memory.hpp"
#include "Utility/LogSys.h"

namespace QBDI {

std::vector<MemoryMap> getCurrentProcessMaps(bool full_path) {
  return getRemoteProcessMaps(getpid(), full_path);
}

std::vector<MemoryMap> getRemoteProcessMaps(QBDI::rword pid, bool full_path) {
  static const int BUFFER_SIZE = 256;
  char line[BUFFER_SIZE] = {0};
  FILE *mapfile = nullptr;
  std::vector<MemoryMap> maps;

  snprintf(line, BUFFER_SIZE, "/proc/%llu/maps", (unsigned long long)pid);
  mapfile = fopen(line, "r");
  QBDI_DEBUG("Querying memory maps from {}", line);
  QBDI_REQUIRE_ACTION(mapfile != nullptr, return maps);

  // Process a memory map line in the form of
  // 00400000-0063c000 r-xp 00000000 fe:01 675628    /usr/bin/vim
  while (fgets(line, BUFFER_SIZE, mapfile) != nullptr) {
    char *ptr = nullptr;
    MemoryMap m;

    // Remove \n
    if ((ptr = strchr(line, '\n')) != nullptr) {
      *ptr = '\0';
    }
    ptr = line;
    QBDI_DEBUG("Parsing line: {}", line);

    // Read range
    m.range.setStart(strtoul(ptr, &ptr, 16));
    ptr++; // '-'
    m.range.setEnd(strtoul(ptr, &ptr, 16));

    // skip the spaces
    while (isspace(*ptr))
      ptr++;

    // Read the permission
    m.permission = QBDI::PF_NONE;
    if (*ptr == 'r')
      m.permission |= QBDI::PF_READ;
    ptr++;
    if (*ptr == 'w')
      m.permission |= QBDI::PF_WRITE;
    ptr++;
    if (*ptr == 'x')
      m.permission |= QBDI::PF_EXEC;
    ptr++;
    ptr++; // skip the protected

    // skip the spaces
    while (isspace(*ptr))
      ptr++;

    // Discard the file offset
    strtoul(ptr, &ptr, 16);

    // skip the spaces
    while (isspace(*ptr))
      ptr++;

    // Discard the device id
    strtoul(ptr, &ptr, 16);
    ptr++; // ':'
    strtoul(ptr, &ptr, 16);

    // skip the spaces
    while (isspace(*ptr))
      ptr++;

    // Discard the inode
    strtoul(ptr, &ptr, 10);

    // skip the spaces
    while (isspace(*ptr))
      ptr++;

    // Get the file name
    if (full_path) {
      m.name = ptr;
    } else {
      if ((ptr = strrchr(ptr, '/')) != nullptr) {
        m.name = ptr + 1;
      } else {
        m.name.clear();
      }
    }

    QBDI_DEBUG("Read new map [0x{:x}, 0x{:x}] {} {}{}{}", m.range.start(),
               m.range.end(), m.name.c_str(),
               (m.permission & QBDI::PF_READ) ? "r" : "-",
               (m.permission & QBDI::PF_WRITE) ? "w" : "-",
               (m.permission & QBDI::PF_EXEC) ? "x" : "-");
    maps.push_back(m);
  }
  fclose(mapfile);
  return maps;
}

} // namespace QBDI
