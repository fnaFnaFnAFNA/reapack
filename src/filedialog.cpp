/* ReaPack: Package manager for REAPER
 * Copyright (C) 2015-2017  Christian Fillion
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "filedialog.hpp"

#include "path.hpp"

#ifndef _WIN32
#include <swell.h>
#endif

auto_string FileDialog::getOpenFileName(HWND parent, HINSTANCE instance,
  const auto_char *title, const Path &initialDir,
  const auto_char *filters, const auto_char *defaultExt)
{
#ifdef _WIN32
  const auto_string &dirPath = make_autostring(initialDir.join());
  auto_char path[4096] = {};

  OPENFILENAME of{sizeof(OPENFILENAME), parent, instance};
  of.lpstrFilter = filters;
  of.lpstrFile = path;
  of.nMaxFile = auto_size(path);
  of.lpstrInitialDir = dirPath.c_str();
  of.lpstrTitle = title;
  of.Flags = OFN_HIDEREADONLY | OFN_EXPLORER | OFN_FILEMUSTEXIST;
  of.lpstrDefExt = defaultExt;

  return GetOpenFileName(&of) ? path : auto_string();
#else
  char *path = BrowseForFiles(title, initialDir.join().c_str(),
    nullptr, false, filters);
  return path ? path : auto_string();
#endif
}

auto_string FileDialog::getSaveFileName(HWND parent, HINSTANCE instance,
  const auto_char *title, const Path &initialDir,
  const auto_char *filters, const auto_char *defaultExt)
{
#ifdef _WIN32
  const auto_string &dirPath = make_autostring(initialDir.join());
  auto_char path[4096] = {};

  OPENFILENAME of{sizeof(OPENFILENAME), parent, instance};
  of.lpstrFilter = filters;
  of.lpstrFile = path;
  of.nMaxFile = auto_size(path);
  of.lpstrInitialDir = dirPath.c_str();
  of.lpstrTitle = title;
  of.Flags = OFN_HIDEREADONLY | OFN_EXPLORER | OFN_OVERWRITEPROMPT;
  of.lpstrDefExt = defaultExt;

  return GetSaveFileName(&of) ? path : auto_string();
#else
  char path[4096] = {};

  if(BrowseForSaveFile(title, initialDir.join().c_str(),
      nullptr, filters, path, sizeof(path)))
    return path;
  else
    return {};
#endif
}
