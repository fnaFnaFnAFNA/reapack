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

#include "reapack.hpp"

#include "about.hpp"
#include "browser.hpp"
#include "config.hpp"
#include "download.hpp"
#include "errors.hpp"
#include "filesystem.hpp"
#include "index.hpp"
#include "manager.hpp"
#include "progress.hpp"
#include "query.hpp"
#include "report.hpp"
#include "richedit.hpp"
#include "transaction.hpp"

#include <reaper_plugin_functions.h>

using namespace std;

const char *ReaPack::VERSION = "1.2beta1";
const char *ReaPack::BUILDTIME = __DATE__ " " __TIME__;

#ifdef _WIN32
// Removes temporary files that could not be removed by an installation task
// (eg. extensions dll that were in use by REAPER).
// Surely there must be a better way...
static void CleanupTempFiles()
{
  const Path &path = Path::prefixRoot(Path::DATA + "*.tmp");
  const auto_string &pattern = make_autostring(path.join());

  WIN32_FIND_DATA fd = {};
  HANDLE handle = FindFirstFile(pattern.c_str(), &fd);

  if(handle == INVALID_HANDLE_VALUE)
    return;

  do {
    auto_string file = pattern;
    file.replace(file.size() - 5, 5, fd.cFileName); // 5 == strlen("*.tmp")
    DeleteFile(file.c_str());
  } while(FindNextFile(handle, &fd));

  FindClose(handle);
}
#endif

std::string ReaPack::resourcePath()
{
#ifdef _WIN32
  // convert from current system encoding to unicode...
  return from_autostring(make_autostring(GetResourcePath(), CP_ACP));
#else
  return GetResourcePath();
#endif
}

ReaPack::ReaPack(REAPER_PLUGIN_HINSTANCE instance)
  : syncAction(), browseAction(), importAction(), configAction(),
    m_tx(nullptr), m_progress(nullptr), m_browser(nullptr), m_manager(nullptr),
    m_about(nullptr), m_instance(instance)
{
  m_mainWindow = GetMainHwnd();
  m_useRootPath = new UseRootPath(resourcePath());

  DownloadContext::GlobalInit();
  RichEdit::Init();

  FS::mkdir(Path::CACHE);

  m_config = new Config;
  m_config->read(Path::prefixRoot(Path::CONFIG));

  if(m_config->isFirstRun())
    manageRemotes();

  registerSelf();

#ifdef _WIN32
  CleanupTempFiles();
#endif
}

ReaPack::~ReaPack()
{
  Dialog::DestroyAll();

  m_config->write();
  delete m_config;

  DownloadContext::GlobalCleanup();

  delete m_useRootPath;
}

int ReaPack::setupAction(const char *name, const ActionCallback &callback)
{
  const int id = plugin_register("command_id", (void *)name);
  m_actions[id] = callback;

  return id;
}

int ReaPack::setupAction(const char *name, const char *desc,
  gaccel_register_t *action, const ActionCallback &callback)
{
  const int id = setupAction(name, callback);

  action->desc = desc;
  action->accel.cmd = id;

  plugin_register("gaccel", action);

  return id;
}

bool ReaPack::execActions(const int id, const int)
{
  if(!m_actions.count(id))
    return false;

  m_actions.at(id)();

  return true;
}

void ReaPack::synchronizeAll()
{
  const vector<Remote> &remotes = m_config->remotes.getEnabled();

  if(remotes.empty()) {
    ShowMessageBox("No repository enabled, nothing to do!", "ReaPack", MB_OK);
    return;
  }

  Transaction *tx = setupTransaction();

  if(!tx)
    return;

  for(const Remote &remote : remotes)
    tx->synchronize(remote);

  tx->runTasks();
}

void ReaPack::setRemoteEnabled(const bool enable, const Remote &remote)
{
  assert(m_tx);

  Remote copy(remote);
  copy.setEnabled(enable);

  m_tx->registerAll(copy);

  m_tx->onFinish([=] {
    if(m_tx->isCancelled())
      return;

    m_config->remotes.add(copy);
    refreshManager();
  });
}

void ReaPack::uninstall(const Remote &remote)
{
  if(remote.isProtected())
    return;

  assert(m_tx);
  m_tx->uninstall(remote);

  m_tx->onFinish([=] {
    if(!m_tx->isCancelled())
      m_config->remotes.remove(remote);
  });
}

void ReaPack::importRemote()
{
  const bool autoClose = m_manager == nullptr;

  manageRemotes();

  if(!m_manager->importRepo() && autoClose)
    m_manager->close();
}

void ReaPack::manageRemotes()
{
  if(m_manager) {
    m_manager->setFocus();
    return;
  }

  m_manager = Dialog::Create<Manager>(m_instance, m_mainWindow, this);
  m_manager->show();

  m_manager->setCloseHandler([=] (INT_PTR) {
    Dialog::Destroy(m_manager);
    m_manager = nullptr;
  });
}

Remote ReaPack::remote(const string &name) const
{
  return m_config->remotes.get(name);
}

void ReaPack::about(const Remote &repo)
{
  Transaction *tx = setupTransaction();
  if(!tx)
    return;

  const vector<Remote> repos = {repo};

  tx->fetchIndexes(repos);
  tx->onFinish([=] {
    const auto &indexes = tx->getIndexes(repos);
    if(!indexes.empty())
      about()->setDelegate(make_shared<AboutIndexDelegate>(indexes.front()));
  });
  tx->runTasks();
}

void ReaPack::aboutSelf()
{
  about(remote("ReaPack"));
}

About *ReaPack::about(const bool instantiate)
{
  if(m_about)
    return m_about;
  else if(!instantiate)
    return nullptr;

  m_about = Dialog::Create<About>(m_instance, m_mainWindow, this);

  m_about->setCloseHandler([=] (INT_PTR) {
    Dialog::Destroy(m_about);
    m_about = nullptr;
  });

  return m_about;
}

Browser *ReaPack::browsePackages()
{
  if(m_browser) {
    m_browser->setFocus();
    return m_browser;
  }
  else if(m_tx) {
    ShowMessageBox(
      "This feature cannot be used while packages are being installed. "
      "Try again later.", "Browse packages", MB_OK
    );
    return nullptr;
  }

  m_browser = Dialog::Create<Browser>(m_instance, m_mainWindow, this);
  m_browser->refresh();
  m_browser->setCloseHandler([=] (INT_PTR) {
    Dialog::Destroy(m_browser);
    m_browser = nullptr;
  });

  return m_browser;
}

Transaction *ReaPack::setupTransaction()
{
  if(m_progress && m_progress->isVisible())
    m_progress->setFocus();

  if(m_tx)
    return m_tx;

  try {
    m_tx = new Transaction(m_config);
  }
  catch(const reapack_error &e) {
    const auto_string &desc = make_autostring(e.what());

    auto_char msg[512];
    auto_snprintf(msg, auto_size(msg),
      AUTO_STR("The following error occurred while creating a transaction:\n\n%s"),
      desc.c_str()
    );

    MessageBox(m_mainWindow, msg, AUTO_STR("ReaPack"), MB_OK);
    return nullptr;
  }

  assert(!m_progress);
  m_progress = Dialog::Create<Progress>(m_instance, m_mainWindow, m_tx->threadPool());

  m_tx->onFinish([=] {
    Dialog::Destroy(m_progress);
    m_progress = nullptr;

    if(!m_tx->isCancelled() && !m_tx->receipt()->empty()) {
      LockDialog managerLock(m_manager);
      LockDialog browserLock(m_browser);

      Dialog::Show<Report>(m_instance, m_mainWindow, *m_tx->receipt());
    }

  });

  m_tx->setObsoleteHandler([=] (vector<Registry::Entry> &entries) {
    LockDialog progressLock(m_progress);
    LockDialog managerLock(m_manager);
    LockDialog browserLock(m_browser);

    return Dialog::Show<ObsoleteQuery>(m_instance, m_mainWindow,
      &entries, &m_config->install.promptObsolete) == IDOK;
  });

  m_tx->setCleanupHandler(bind(&ReaPack::teardownTransaction, this));

  return m_tx;
}

void ReaPack::teardownTransaction()
{
  delete m_tx;
  m_tx = nullptr;

  // Update the browser only after the transaction is deleted because
  // it must be able to start a new one to load the indexes
  refreshBrowser();
}

void ReaPack::refreshManager()
{
  if(m_manager)
    m_manager->refresh();
}

void ReaPack::refreshBrowser()
{
  if(m_browser)
    m_browser->refresh();
}

void ReaPack::registerSelf()
{
  // hard-coding galore!
  Index ri("ReaPack");
  Category cat("Extensions", &ri);
  Package pkg(Package::ExtensionType, "ReaPack.ext", &cat);
  Version ver(VERSION, &pkg);
  ver.setAuthor("cfillion");
  ver.addSource(new Source(REAPACK_FILE, "dummy url", &ver));

  try {
    Registry reg(Path::prefixRoot(Path::REGISTRY));
    reg.push(&ver);
    reg.commit();
  }
  catch(const reapack_error &e) {
    const auto_string &desc = make_autostring(e.what());

    auto_char msg[255];
    auto_snprintf(msg, auto_size(msg),
      AUTO_STR("ReaPack could not register itself! Please report this issue.\n\n")
      AUTO_STR("Error description: %s"), desc.c_str());

    MessageBox(Splash_GetWnd(), msg, AUTO_STR("ReaPack"), MB_OK);
  }
}
