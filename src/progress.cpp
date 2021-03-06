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

#include "progress.hpp"

#include "thread.hpp"
#include "resource.hpp"

using namespace std;

Progress::Progress(ThreadPool *pool)
  : Dialog(IDD_PROGRESS_DIALOG),
    m_pool(pool), m_label(nullptr), m_progress(nullptr),
    m_done(0), m_total(0)
{
  m_pool->onPush(bind(&Progress::addTask, this, placeholders::_1));
}

void Progress::onInit()
{
  Dialog::onInit();

  m_label = getControl(IDC_LABEL);
  m_progress = GetDlgItem(handle(), IDC_PROGRESS);

  SetWindowText(m_label, AUTO_STR("Initializing..."));
}

void Progress::onCommand(const int id, int)
{
  switch(id) {
  case IDCANCEL:
    m_pool->abort();

    // don't wait until the current downloads are finished
    // before getting out of the user way
    hide();
    break;
  }
}

void Progress::onTimer(const int id)
{
  show();
  stopTimer(id);
}

void Progress::addTask(ThreadTask *task)
{
  m_total++;
  updateProgress();

  if(!isVisible())
    startTimer(100);

  task->onStart([=] {
    m_current = make_autostring(task->summary());
    updateProgress();
  });

  task->onFinish([=] {
    m_done++;
    updateProgress();
  });
}

void Progress::updateProgress()
{
  auto_char position[32];
  auto_snprintf(position, auto_size(position), AUTO_STR("%d of %d"),
    min(m_done + 1, m_total), m_total);

  auto_char label[1024];
  auto_snprintf(label, auto_size(label), m_current.c_str(), position);

  SetWindowText(m_label, label);

  const double pos = (double)(min(m_done+1, m_total)) / max(2, m_total);
  const int percent = (int)(pos * 100);

  auto_char title[255];
  auto_snprintf(title, auto_size(title),
    AUTO_STR("ReaPack: Operation in progress (%d%%)"), percent);

  SendMessage(m_progress, PBM_SETPOS, percent, 0);
  SetWindowText(handle(), title);
}
