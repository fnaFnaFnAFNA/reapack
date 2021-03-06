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

#include "archive.hpp"

#include "config.hpp"
#include "errors.hpp"
#include "filesystem.hpp"
#include "index.hpp"
#include "path.hpp"
#include "reapack.hpp"
#include "transaction.hpp"

#include <boost/format.hpp>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <zlib/zip.h>
#include <zlib/unzip.h>
#include <zlib/ioapi.h>

using namespace boost;
using namespace std;

static const Path ARCHIVE_TOC = Path("toc");
static const size_t BUFFER_SIZE = 4096;

#ifdef _WIN32
static void *wide_fopen(voidpf, const void *filename, int mode)
{
  const wchar_t *fopen_mode = nullptr;

  if((mode & ZLIB_FILEFUNC_MODE_READWRITEFILTER) == ZLIB_FILEFUNC_MODE_READ)
    fopen_mode = L"rb";
  else if(mode & ZLIB_FILEFUNC_MODE_EXISTING)
    fopen_mode = L"r+b";
  else if(mode & ZLIB_FILEFUNC_MODE_CREATE)
    fopen_mode = L"wb";

  FILE *file = nullptr;

  if(filename && fopen_mode)
    _wfopen_s(&file, static_cast<const wchar_t *>(filename), fopen_mode);

  return file;
}
#endif

struct ImportArchive {
  void importRemote(const string &);
  void importPackage(const string &);

  ArchiveReaderPtr m_reader;
  RemoteList *m_remotes;
  Transaction *m_tx;
  IndexPtr m_lastIndex;
};

void Archive::import(const auto_string &path, ReaPack *reapack)
{
  ImportArchive state{make_shared<ArchiveReader>(path), &reapack->config()->remotes};

  stringstream toc;
  if(const int err = state.m_reader->extractFile(ARCHIVE_TOC, toc))
    throw reapack_error(format("Cannot locate the table of contents (%d)") % err);

  // starting import, do not abort process (eg. by throwing) at this point
  if(!(state.m_tx = reapack->setupTransaction()))
    return;

  string line;
  while(getline(toc, line)) {
    if(line.size() <= 5) // 5 is the length of the line type prefix
      continue;

    const string &data = line.substr(5);

    try {
      switch(line[0]) {
      case 'R':
        state.importRemote(data);
        break;
      case 'P':
        state.importPackage(data);
        break;
      default:
        throw reapack_error(format("Unknown token '%s' (skipping)")
          % line.substr(0, 4));
      }
    }
    catch(const reapack_error &e) {
      state.m_tx->receipt()->addError({e.what(), from_autostring(path)});
    }
  }

  reapack->config()->write();
  state.m_tx->runTasks();
}

void ImportArchive::importRemote(const string &data)
{
  m_lastIndex = nullptr; // clear the previous repository
  Remote remote = Remote::fromString(data);

  if(const int err = m_reader->extractFile(Index::pathFor(remote.name()))) {
    throw reapack_error(format("Failed to extract index of %s (%d)")
      % remote.name() % err);
  }

  const Remote &original = m_remotes->get(remote.name());
  if(original.isProtected()) {
    remote.setUrl(original.url());
    remote.protect();
  }

  m_remotes->add(remote);
  m_lastIndex = Index::load(remote.name());
}

void ImportArchive::importPackage(const string &data)
{
  // don't report an error if the index isn't loaded assuming we already
  // did when failing to import the repository above
  if(!m_lastIndex)
    return;

  string categoryName, packageName, versionName;
  bool pinned;

  istringstream stream(data);
  stream
    >> quoted(categoryName) >> quoted(packageName) >> quoted(versionName)
    >> pinned;

  const Package *pkg = m_lastIndex->find(categoryName, packageName);
  const Version *ver = pkg ? pkg->findVersion(versionName) : nullptr;

  if(!ver) {
    throw reapack_error(format("%s/%s/%s v%s cannot be found or is"
      " incompatible with your operating system.")
      % m_lastIndex->name() % categoryName % packageName % versionName);
  }

  m_tx->install(ver, pinned, m_reader);
}

ArchiveReader::ArchiveReader(const auto_string &path)
{
  zlib_filefunc64_def filefunc;
  fill_fopen64_filefunc(&filefunc);
#ifdef _WIN32
  filefunc.zopen64_file = wide_fopen;
#endif

  m_zip = unzOpen2_64(reinterpret_cast<const char *>(path.c_str()), &filefunc);

  if(!m_zip)
    throw reapack_error(FS::lastError().c_str());
}

ArchiveReader::~ArchiveReader()
{
  unzClose(m_zip);
}

int ArchiveReader::extractFile(const Path &path)
{
  ofstream stream;

  if(FS::open(stream, path))
    return extractFile(path, stream);
  else
    throw reapack_error(format("%s: %s") % path.join() % FS::lastError());
}

int ArchiveReader::extractFile(const Path &path, ostream &stream) noexcept
{
  int status = unzLocateFile(m_zip, path.join('/').c_str(), false);
  if(status != UNZ_OK)
    return status;

  status = unzOpenCurrentFile(m_zip);
  if(status != UNZ_OK)
    return status;

  string buffer(BUFFER_SIZE, 0);

  const auto readChunk = [&] {
    return unzReadCurrentFile(m_zip, &buffer[0], (int)buffer.size());
  };

  while(const int len = readChunk()) {
    if(len < 0)
      return len; // read error

    stream.write(&buffer[0], len);
  }

  return unzCloseCurrentFile(m_zip);
}

FileExtractor::FileExtractor(const Path &target, const ArchiveReaderPtr &reader)
  : m_path(target), m_reader(reader)
{
  setSummary("Extracting %s: " + target.join());
}

void FileExtractor::run(DownloadContext *)
{
  if(aborted()) {
    finish(Aborted, {"cancelled", m_path.target().join()});
    return;
  }

  ThreadNotifier::get()->notify({this, Running});

  ofstream stream;
  if(!FS::open(stream, m_path.temp())) {
    finish(Failure, {FS::lastError(), m_path.temp().join()});
    return;
  }

  const int error = m_reader->extractFile(m_path.target(), stream);
  stream.close();

  if(error) {
    const format &msg = format("Failed to extract file (%d)") % error;
    finish(Failure, {msg.str(), m_path.target().join()});
  }
  else
    finish(Success);
}

size_t Archive::create(const auto_string &path, vector<string> *errors,
  ThreadPool *pool, ReaPack *reapack)
{
  size_t count = 0;
  vector<ThreadTask *> jobs;

  stringstream toc;
  Registry reg(Path::prefixRoot(Path::REGISTRY));

  ArchiveWriterPtr writer = make_shared<ArchiveWriter>(path);

  const auto compress = [&] (const Path &path) {
    if(FS::exists(path))
      jobs.push_back(new FileCompressor(path, writer));
    else {
      const auto fmt = format("%s (%s)") % path.join() % FS::lastError();
      errors->push_back(fmt.str());
    }
  };

  for(const Remote &remote : reapack->config()->remotes.getEnabled()) {
    bool addedRemote = false;

    for(const Registry::Entry &entry : reg.getEntries(remote.name())) {
      ++count;

      if(!addedRemote) {
        toc << "REPO " << remote.toString() << '\n';
        compress(Index::pathFor(remote.name()));
        addedRemote = true;
      }

      toc << "PACK "
        << quoted(entry.category) << '\x20'
        << quoted(entry.package) << '\x20'
        << quoted(entry.version.toString()) << '\x20'
        << entry.pinned << '\n'
      ;

      for(const Registry::File &file : reg.getFiles(entry))
        compress(file.path);
    }
  }

  writer->addFile(ARCHIVE_TOC, toc);

  // Start after we've written the table of contents in the main thread
  // because we cannot safely write into the zip from more than one
  // thread at the same time.
  for(ThreadTask *job : jobs)
    pool->push(job);

  return count;
}

ArchiveWriter::ArchiveWriter(const auto_string &path)
{
  zlib_filefunc64_def filefunc;
  fill_fopen64_filefunc(&filefunc);
#ifdef _WIN32
  filefunc.zopen64_file = wide_fopen;
#endif

  m_zip = zipOpen2_64(reinterpret_cast<const char *>(path.c_str()),
    APPEND_STATUS_CREATE, nullptr, &filefunc);

  if(!m_zip)
    throw reapack_error(FS::lastError().c_str());
}

ArchiveWriter::~ArchiveWriter()
{
  zipClose(m_zip, nullptr);
}

int ArchiveWriter::addFile(const Path &path)
{
  ifstream stream;

  if(FS::open(stream, path))
    return addFile(path, stream);
  else
    throw reapack_error(format("%s: %s") % path.join() % FS::lastError());
}

int ArchiveWriter::addFile(const Path &path, istream &stream) noexcept
{
  const int status = zipOpenNewFileInZip(m_zip, path.join('/').c_str(), nullptr,
    nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED, Z_DEFAULT_COMPRESSION);

  if(status != ZIP_OK)
    return status;

  string buffer(BUFFER_SIZE, 0);

  const auto readChunk = [&] {
    stream.read(&buffer[0], buffer.size());
    return (int)stream.gcount();
  };

  while(const int len = readChunk()) {
    if(len < 0)
      return len; // write error

    zipWriteInFileInZip(m_zip, &buffer[0], len);
  }

  return zipCloseFileInZip(m_zip);
}

FileCompressor::FileCompressor(const Path &target, const ArchiveWriterPtr &writer)
  : m_path(target), m_writer(writer)
{
  setSummary("Compressing %s: " + target.join());
}

void FileCompressor::run(DownloadContext *)
{
  if(aborted()) {
    finish(Aborted, {"cancelled", m_path.join()});
    return;
  }

  ThreadNotifier::get()->notify({this, Running});

  ifstream stream;
  if(!FS::open(stream, m_path)) {
    finish(Failure, {FS::lastError(), m_path.join()});
    return;
  }

  const int error = m_writer->addFile(m_path, stream);
  stream.close();

  if(error) {
    const format &msg = format("Failed to compress file (%d)") % error;
    finish(Failure, {msg.str(), m_path.join()});
  }
  else
    finish(Success);
}
