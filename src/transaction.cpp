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

#include "transaction.hpp"

#include "archive.hpp"
#include "config.hpp"
#include "download.hpp"
#include "errors.hpp"
#include "filesystem.hpp"
#include "index.hpp"
#include "remote.hpp"
#include "task.hpp"

#include <reaper_plugin_functions.h>

using namespace std;

static const time_t STALE_THRESHOLD = 7 * 24 * 3600;

Transaction::Transaction(Config *config)
  : m_isCancelled(false), m_config(config),
    m_registry(Path::prefixRoot(Path::REGISTRY))
{
  // don't keep pre-install pushes (for conflict checks); released in runTasks
  m_registry.savepoint();

  m_threadPool.onPush([this] (ThreadTask *task) {
    task->onFinish([=] {
      if(task->state() == ThreadTask::Failure)
        m_receipt.addError(task->error());
    });
  });

  m_threadPool.onAbort([this] {
    m_isCancelled = true;
    queue<HostTicket>().swap(m_regQueue);
  });

  // run tasks after fetching indexes
  m_threadPool.onDone(bind(&Transaction::runTasks, this));
}

void Transaction::synchronize(const Remote &remote,
  const boost::optional<bool> forceAutoInstall)
{
  if(m_syncedRemotes.count(remote.name()))
    return;

  m_syncedRemotes.insert(remote.name());

  InstallOpts opts = m_config->install;

  if(forceAutoInstall)
    opts.autoInstall = *forceAutoInstall;
  else if(!boost::logic::indeterminate(remote.autoInstall()))
    opts.autoInstall = remote.autoInstall();

  fetchIndex(remote, true, [=] (const IndexPtr &ri) {
    for(const Package *pkg : ri->packages())
      synchronize(pkg, opts);

    if(m_config->install.promptObsolete && !remote.isProtected()) {
      for(const Registry::Entry &entry : m_registry.getEntries(ri->name())) {
        if(!ri->find(entry.category, entry.package))
          m_obsolete.insert(entry);
      }
    }
  });
}

void Transaction::synchronize(const Package *pkg, const InstallOpts &opts)
{
  const auto &regEntry = m_registry.getEntry(pkg);

  if(!regEntry && !opts.autoInstall)
    return;

  const Version *latest = pkg->lastVersion(opts.bleedingEdge, regEntry.version);

  // don't crash nor install a pre-release if autoInstall is on with
  // bleedingEdge mode off and there is no stable release
  if(!latest)
    return;

  if(regEntry.version == latest->name()) {
    if(allFilesExists(latest->files()))
      return; // latest version is really installed, nothing to do here!
  }
  else if(regEntry.pinned || latest->name() < regEntry.version)
    return;

  m_nextQueue.push(make_shared<InstallTask>(latest, false, regEntry, nullptr, this));
}

void Transaction::fetchIndexes(const vector<Remote> &remotes, const bool stale)
{
  for(const Remote &remote : remotes)
    fetchIndex(remote, stale);
}

vector<IndexPtr> Transaction::getIndexes(const vector<Remote> &remotes) const
{
  vector<IndexPtr> indexes;

  for(const Remote &remote : remotes) {
    const auto it = m_indexes.find(remote.name());
    if(it != m_indexes.end())
      indexes.push_back(it->second);
  }

  return indexes;
}

void Transaction::fetchIndex(const Remote &remote, const bool stale,
  const function<void (const IndexPtr &)> &cb)
{
  const auto load = [=] {
    const IndexPtr ri = loadIndex(remote);
    if(cb && ri)
      cb(ri);
  };

  const Path &path = Index::pathFor(remote.name());
  time_t mtime = 0, now = time(nullptr);
  FS::mtime(path, &mtime);

  if(!stale && mtime > now - STALE_THRESHOLD) {
    load();
    return;
  }

  auto dl = new FileDownload(path, remote.url(),
    m_config->network, Download::NoCacheFlag);
  dl->setName(remote.name());

  dl->onFinish([=] {
    if(!dl->save())
      m_receipt.addError({FS::lastError(), dl->path().target().join()});

    if(FS::exists(path))
      load(); // try to load anyway, even on failure
  });

  m_threadPool.push(dl);
}

IndexPtr Transaction::loadIndex(const Remote &remote)
{
  const auto it = m_indexes.find(remote.name());
  if(it != m_indexes.end())
    return it->second;

  try {
    const IndexPtr &ri = Index::load(remote.name());
    m_indexes[remote.name()] = ri;
    return ri;
  }
  catch(const reapack_error &e) {
    m_receipt.addError({
      "Couldn't load repository: " + string(e.what()), remote.name()});
    return nullptr;
  }
}

void Transaction::install(const Version *ver,
  const bool pin, const ArchiveReaderPtr &reader)
{
  const auto &oldEntry = m_registry.getEntry(ver->package());
  m_nextQueue.push(make_shared<InstallTask>(ver, pin, oldEntry, reader, this));
}

void Transaction::registerAll(const Remote &remote)
{
  const vector<Registry::Entry> &entries = m_registry.getEntries(remote.name());

  for(const auto &entry : entries)
    registerAll(remote.isEnabled(), entry);

  if(!remote.isEnabled())
    inhibit(remote);
}

void Transaction::setPinned(const Registry::Entry &entry, const bool pinned)
{
  m_nextQueue.push(make_shared<PinTask>(entry, pinned, this));
}

void Transaction::uninstall(const Remote &remote)
{
  inhibit(remote);

  const Path &indexPath = Index::pathFor(remote.name());

  if(FS::exists(indexPath)) {
    if(!FS::remove(indexPath))
      m_receipt.addError({FS::lastError(), indexPath.join()});
  }

  for(const auto &entry : m_registry.getEntries(remote.name()))
    uninstall(entry);
}

void Transaction::uninstall(const Registry::Entry &entry)
{
  m_nextQueue.push(make_shared<UninstallTask>(entry, this));
}

bool Transaction::runTasks()
{
  if(!m_nextQueue.empty()) {
    m_taskQueues.push(m_nextQueue);
    TaskQueue().swap(m_nextQueue);
  }

  if(!commitTasks())
    return false; // we're downloading indexes for synchronization
  else if(m_isCancelled) {
    finish();
    return true;
  }

  promptObsolete();

  while(!m_taskQueues.empty()) {
    m_registry.savepoint();

    auto &queue = m_taskQueues.front();

    while(!queue.empty()) {
      const TaskPtr &task = queue.top();

      if(task->start())
        m_runningTasks.push(task);

      queue.pop();
    }

    m_registry.restore();
    m_taskQueues.pop();

    if(!commitTasks()) // if the tasks didn't finish immediately (downloading)
      return false;
  }

  // we're done!
  m_registry.commit();
  registerQueued();

  finish();

  return true;
}

bool Transaction::commitTasks()
{
  // wait until all running tasks are ready
  if(!m_threadPool.idle())
    return false;

  // finish current tasks
  while(!m_runningTasks.empty()) {
    if(m_isCancelled)
      m_runningTasks.front()->rollback();
    else
      m_runningTasks.front()->commit();

    m_runningTasks.pop();
  }

  return true;
}

void Transaction::finish()
{
  m_onFinish();
  m_cleanupHandler();
}

bool Transaction::allFilesExists(const set<Path> &list) const
{
  for(const Path &path : list) {
    if(!FS::exists(path))
      return false;
  }

  return true;
}

void Transaction::registerAll(const bool add, const Registry::Entry &entry)
{
  // don't actually do anything until commit() – which will calls registerQueued
  for(const Registry::File &file : m_registry.getMainFiles(entry))
    registerFile({add, entry, file});
}

void Transaction::registerQueued()
{
  while(!m_regQueue.empty()) {
    const HostTicket &reg = m_regQueue.front();

    // don't register in host if the remote got disabled meanwhile
    if(reg.add && m_inhibited.count(reg.entry.remote) > 0) {
      m_regQueue.pop();
      return;
    }

    switch(reg.file.type) {
    case Package::ScriptType:
      registerScript(reg, m_regQueue.size() == 1);
      break;
    default:
      break;
    }

    m_regQueue.pop();
  }
}

void Transaction::registerScript(const HostTicket &reg, const bool isLastCall)
{
  static const map<Source::Section, int> sectionMap{
    {Source::MainSection, 0},
    {Source::MIDIEditorSection, 32060},
    {Source::MIDIInlineEditorSection, 32062},
  };

  if(!AddRemoveReaScript || !reg.file.sections)
    return; // do nothing if REAPER < v5.12 and skip non-main files

  const string &fullPath = Path::prefixRoot(reg.file.path).join();

  vector<int> sections;

  for(const auto &pair : sectionMap) {
    if(reg.file.sections & pair.first)
      sections.push_back(pair.second);
  }

  assert(!sections.empty()); // is a section missing in sectionMap?

  bool enableError = reg.add;
  auto it = sections.begin();

  while(true) {
    const int section = *it++;
    const bool isLastSection = it == sections.end();

    const int id = AddRemoveReaScript(reg.add, section, fullPath.c_str(),
      isLastCall && isLastSection);

    if(!id && enableError) {
      m_receipt.addError({"This script could not be registered in REAPER.",
        reg.file.path.join()});
      enableError = false;
    }

    if(isLastSection)
      break;
  }
}

void Transaction::inhibit(const Remote &remote)
{
  // prevents index post-download callbacks from being called
  // AND prevents files from this remote from being registered in REAPER
  // (UNregistering is not affected)

  const auto it = m_syncedRemotes.find(remote.name());
  if(it != m_syncedRemotes.end())
    m_syncedRemotes.erase(it);

  m_inhibited.insert(remote.name());
}

void Transaction::promptObsolete()
{
  if(!m_config->install.promptObsolete || m_obsolete.empty())
    return;

  vector<Registry::Entry> selected;
  selected.insert(selected.end(), m_obsolete.begin(), m_obsolete.end());
  m_obsolete.clear();

  if(!m_promptObsolete(selected) || selected.empty())
    return;

  if(m_taskQueues.empty())
    m_taskQueues.push(TaskQueue());

  for(const auto &entry : selected)
    m_taskQueues.back().push(make_shared<UninstallTask>(entry, this));
}
