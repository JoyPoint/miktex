/* PackageManagerImpl.cpp: MiKTeX Package Manager

   Copyright (C) 2001-2018 Christian Schenk

   This file is part of MiKTeX Package Manager.

   MiKTeX Package Manager is free software; you can redistribute it
   and/or modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2, or
   (at your option) any later version.

   MiKTeX Package Manager is distributed in the hope that it will be
   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with MiKTeX Package Manager; if not, write to the Free
   Software Foundation, 59 Temple Place - Suite 330, Boston, MA
   02111-1307, USA. */

#include "config.h"

#include <fstream>
#include <future>
#include <locale>
#include <stack>
#include <unordered_set>

#include <fmt/format.h>

#include <miktex/Core/Directory>
#include <miktex/Core/DirectoryLister>
#include <miktex/Core/Environment>
#include <miktex/Core/PathNameParser>
#include <miktex/Core/Registry>
#include <miktex/Core/TemporaryDirectory>
#include <miktex/Core/Uri>

#include <miktex/Trace/Trace>

#include "internal.h"
#include "PackageManagerImpl.h"
#include "PackageInstallerImpl.h"
#include "PackageIteratorImpl.h"
#include "TpmParser.h"

using namespace std;

using namespace MiKTeX::Core;
using namespace MiKTeX::Extractor;
using namespace MiKTeX::Packages;
using namespace MiKTeX::Trace;
using namespace MiKTeX::Util;

using namespace MiKTeX::Packages::D6AAD62216146D44B580E92711724B78;

string PackageManagerImpl::proxyUser;
string PackageManagerImpl::proxyPassword;

#if defined(MIKTEX_WINDOWS) && USE_LOCAL_SERVER
bool PackageManagerImpl::localServer = false;
#endif

PackageManager::~PackageManager() noexcept
{
}

PackageManagerImpl::PackageManagerImpl(const PackageManager::InitInfo& initInfo) :
  trace_error(TraceStream::Open(MIKTEX_TRACE_ERROR, initInfo.traceCallback)),
  trace_mpm(TraceStream::Open(MIKTEX_TRACE_MPM, initInfo.traceCallback)),
  repositories(webSession)
{
  trace_mpm->WriteFormattedLine("libmpm", T_("initializing MPM library version %s"), MIKTEX_COMPONENT_VERSION_STR);
}

PackageManagerImpl::~PackageManagerImpl()
{
  try
  {
    Dispose();
  }
  catch (const exception &)
  {
  }
}

shared_ptr<PackageManager> PackageManager::Create(const PackageManager::InitInfo& initInfo)
{
  return make_shared<PackageManagerImpl>(initInfo);
}

void PackageManagerImpl::Dispose()
{
  ClearAll();
  if (webSession != nullptr)
  {
    webSession->Dispose();
    webSession = nullptr;
  }
}

unique_ptr<PackageInstaller> PackageManagerImpl::CreateInstaller()
{
  return make_unique<PackageInstallerImpl>(shared_from_this());
}

unique_ptr<PackageIterator> PackageManagerImpl::CreateIterator()
{
  return make_unique<PackageIteratorImpl>(shared_from_this());
}

void PackageManagerImpl::LoadVariablePackageTable()
{
  // only load once
  if (commonVariablePackageTable != nullptr)
  {
    return;
  }

  commonVariablePackageTable = Cfg::Create();

  PathName pathCommonPackagesIni(session->GetSpecialPath(SpecialPath::CommonInstallRoot), MIKTEX_PATH_PACKAGES_INI);

  if (File::Exists(pathCommonPackagesIni))
  {
    trace_mpm->WriteFormattedLine("libmpm", T_("loading common variable package table (%s)"), Q_(pathCommonPackagesIni));
    commonVariablePackageTable->Read(pathCommonPackagesIni);
  }

  commonVariablePackageTable->SetModified(false);

  PathName pathUserPackagesIni(session->GetSpecialPath(SpecialPath::UserInstallRoot), MIKTEX_PATH_PACKAGES_INI);

  if (!session->IsAdminMode() && (pathCommonPackagesIni.Canonicalize() != pathUserPackagesIni.Canonicalize()))
  {
    userVariablePackageTable = Cfg::Create();
    if (File::Exists(pathUserPackagesIni))
    {
      trace_mpm->WriteFormattedLine("libmpm", T_("loading user variable package table (%s)"), Q_(pathUserPackagesIni));
      userVariablePackageTable->Read(pathUserPackagesIni);
    }
    userVariablePackageTable->SetModified(false);
  }
}

void PackageManagerImpl::FlushVariablePackageTable()
{
  if (commonVariablePackageTable != nullptr
    && commonVariablePackageTable->IsModified())
  {
    PathName pathPackagesIni(session->GetSpecialPath(SpecialPath::CommonInstallRoot), MIKTEX_PATH_PACKAGES_INI);
    trace_mpm->WriteFormattedLine("libmpm", T_("flushing common variable package table (%s)"), Q_(pathPackagesIni));
    commonVariablePackageTable->Write(pathPackagesIni);
  }
  if (userVariablePackageTable != nullptr && userVariablePackageTable->IsModified())
  {
    PathName pathPackagesIni(session->GetSpecialPath(SpecialPath::UserInstallRoot), MIKTEX_PATH_PACKAGES_INI);
    trace_mpm->WriteFormattedLine("libmpm", T_("flushing user variable package table (%s)"), Q_(pathPackagesIni));
    userVariablePackageTable->Write(pathPackagesIni);
  }
}

bool PackageManagerImpl::IsRemovable(const string& deploymentName)
{
  bool ret;
  LoadVariablePackageTable();
  string str;
  if (session->IsAdminMode())
  {
    // administrator can remove system-wide packages
    ret = GetCommonTimeInstalled(deploymentName) != 0;
  }
  else
  {
    // user can remove private packages
    if (session->GetSpecialPath(SpecialPath::CommonInstallRoot).Canonicalize() == session->GetSpecialPath(SpecialPath::UserInstallRoot).Canonicalize())
    {
      ret = GetTimeInstalled(deploymentName) != 0;
    }
    else
    {
      ret = GetUserTimeInstalled(deploymentName) != 0;
    }
  }
  return ret;
}

time_t PackageManagerImpl::GetUserTimeInstalled(const string& deploymentName)
{
  if (session->IsAdminMode())
  {
    MIKTEX_UNEXPECTED();
  }
  LoadVariablePackageTable();
  string str;
  if (userVariablePackageTable != nullptr && userVariablePackageTable->TryGetValue(deploymentName, "TimeInstalled", str))
  {
    return std::stoi(str);
  }
  else
  {
    return 0;
  }
}

time_t PackageManagerImpl::GetCommonTimeInstalled(const string& deploymentName)
{
  LoadVariablePackageTable();
  string str;
  if (commonVariablePackageTable != nullptr && commonVariablePackageTable->TryGetValue(deploymentName, "TimeInstalled", str))
  {
    return std::stoi(str);
  }
  else
  {
    return 0;
  }
}

time_t PackageManagerImpl::GetTimeInstalled(const string& deploymentName)
{
  LoadVariablePackageTable();
  string str;
  if ((!session->IsAdminMode() && userVariablePackageTable != nullptr && userVariablePackageTable->TryGetValue(deploymentName, "TimeInstalled", str))
    || commonVariablePackageTable->TryGetValue(deploymentName, "TimeInstalled", str))
  {
    return std::stoi(str);
  }
  else
  {
    return 0;
  }
}

bool PackageManagerImpl::IsPackageInstalled(const string& deploymentName)
{
  return GetTimeInstalled(deploymentName) > 0;
}

bool PackageManagerImpl::IsPackageObsolete(const string& deploymentName)
{
  LoadVariablePackageTable();
  string str;
  if ((!session->IsAdminMode()
    && userVariablePackageTable != nullptr
    && userVariablePackageTable->TryGetValue(deploymentName, "Obsolete", str))
    || commonVariablePackageTable->TryGetValue(deploymentName, "Obsolete", str))
  {
    return std::stoi(str) != 0;
  }
  else
  {
    return false;
  }
}

void PackageManagerImpl::DeclarePackageObsolete(const string& deploymentName, bool obsolete)
{
  LoadVariablePackageTable();
  if (session->IsAdminMode() || userVariablePackageTable == nullptr)
  {
    commonVariablePackageTable->PutValue(deploymentName, "Obsolete", (obsolete ? "1" : "0"));
  }
  else
  {
    userVariablePackageTable->PutValue(deploymentName, "Obsolete", (obsolete ? "1" : "0"));
  }
}

void PackageManagerImpl::SetTimeInstalled(const string& deploymentName, time_t timeInstalled)
{
  LoadVariablePackageTable();
  if (session->IsAdminMode() || userVariablePackageTable == nullptr)
  {
    if (timeInstalled == 0)
    {
      commonVariablePackageTable->DeleteKey(deploymentName);
    }
    else
    {
      commonVariablePackageTable->PutValue(deploymentName, "TimeInstalled", std::to_string(timeInstalled));
    }
  }
  else
  {
    if (timeInstalled == 0)
    {
      userVariablePackageTable->DeleteKey(deploymentName);
    }
    else
    {
      userVariablePackageTable->PutValue(deploymentName, "TimeInstalled", std::to_string(timeInstalled));
    }
  }
}

void PackageManagerImpl::SetReleaseState(const string& deploymentName, RepositoryReleaseState releaseState)
{
  LoadVariablePackageTable();
  if (session->IsAdminMode() || userVariablePackageTable == nullptr)
  {
    commonVariablePackageTable->PutValue(deploymentName, "ReleaseState", releaseState == RepositoryReleaseState::Next ? "next" : releaseState == RepositoryReleaseState::Stable ? "stable" : "");
  }
  else
  {
    userVariablePackageTable->PutValue(deploymentName, "ReleaseState", releaseState == RepositoryReleaseState::Next ? "next" : releaseState == RepositoryReleaseState::Stable ? "stable" : "");
  }
}

RepositoryReleaseState PackageManagerImpl::GetReleaseState(const string& deploymentName)
{
  LoadVariablePackageTable();
  string str;
  if ((!session->IsAdminMode() && userVariablePackageTable != nullptr && userVariablePackageTable->TryGetValue(deploymentName, "ReleaseState", str))
    || commonVariablePackageTable->TryGetValue(deploymentName, "ReleaseState", str))
  {
    if (str == "stable")
    {
      return RepositoryReleaseState::Stable;
    }
    else if (str == "next")
    {
      return RepositoryReleaseState::Next;
    }
  }
  return RepositoryReleaseState::Unknown;
}

void PackageManagerImpl::IncrementFileRefCounts(const vector<string>& files)
{
  for (const string& file : files)
  {
    ++installedFileInfoTable[file].refCount;
#if POLLUTE_THE_DEBUG_STREAM
    if (installedFileInfoTable[file].refCount >= 2)
    {
      trace_mpm->WriteFormattedLine("libmpm", T_("%s: ref count > 1"), Q_(file));
    }
#endif
  }
}

void PackageManagerImpl::IncrementFileRefCounts(const string& deploymentName)
{
  NeedInstalledFileInfoTable();
  const PackageInfo& pi = packageTable[deploymentName];
  IncrementFileRefCounts(pi.runFiles);
  IncrementFileRefCounts(pi.docFiles);
  IncrementFileRefCounts(pi.sourceFiles);
}

PackageInfo* PackageManagerImpl::DefinePackage(const string& deploymentName, const PackageInfo& packageInfo)
{
  pair<PackageDefinitionTable::iterator, bool> p = packageTable.insert(make_pair(deploymentName, packageInfo));
  p.first->second.deploymentName = deploymentName;
  if (session->IsMiKTeXDirect())
  {
    // installed from the start
    p.first->second.isRemovable = false;
    p.first->second.isObsolete = false;
    p.first->second.timeInstalled = packageInfo.timePackaged;
  }
  else
  {
    p.first->second.isRemovable = IsRemovable(deploymentName);
    p.first->second.isObsolete = IsPackageObsolete(deploymentName);
    p.first->second.timeInstalled = GetTimeInstalled(deploymentName);
    if (p.first->second.IsInstalled())
    {
      p.first->second.releaseState = GetReleaseState(deploymentName);
    }
  }
  return &(p.first->second);
}

void PackageManagerImpl::ParseAllPackageDefinitionFilesInDirectory(const PathName& directory)
{
  trace_mpm->WriteFormattedLine("libmpm", T_("searching %s for package definition files"), Q_(directory));

  if (!Directory::Exists(directory))
  {
    trace_mpm->WriteFormattedLine("libmpm", T_("package definition directory (%s) does not exist"), Q_(directory));
    return;
  }

  unique_ptr<DirectoryLister> pLister = DirectoryLister::Open(directory, "*" MIKTEX_PACKAGE_DEFINITION_FILE_SUFFIX);

  vector<future<PackageInfo>> futurePackageInfoTable;

  // parse package definition files
  if (((int)ASYNC_LAUNCH_POLICY & (int)launch::async) != 0)
  {
    const size_t maxPackageFiles = 4000;
    File::SetMaxOpen(maxPackageFiles);
  }
  unsigned count = 0;
  DirectoryEntry direntry;
  while (pLister->GetNext(direntry))
  {
    PathName name(direntry.name);

    // get deployment name
    string deploymentName = name.GetFileNameWithoutExtension().ToString();

    // ignore redefinition
    if (packageTable.find(deploymentName) != packageTable.end())
    {
#if 0
      trace_mpm->WriteFormattedLine("libmpm", T_("%s: ignoring redefinition"), deploymentName.c_str());
#endif
      continue;
    }

    // parse package definition file
    futurePackageInfoTable.push_back(async(ASYNC_LAUNCH_POLICY, [](const PathName& path)
    {
      unique_ptr<TpmParser> tpmParser = TpmParser::Create();
      tpmParser->Parse(path);
      return tpmParser->GetPackageInfo();
    }, PathName(directory, name)));
  }
  pLister->Close();

  for (future<PackageInfo>& fpi : futurePackageInfoTable)
  {
    PackageInfo packageInfo = fpi.get();

#if IGNORE_OTHER_SYSTEMS
    string targetSystems = packageInfo.targetSystem;
    if (targetSystems != "" && !StringUtil::Contains(targetSystems.c_str(), MIKTEX_SYSTEM_TAG))
    {
      trace_mpm->WriteFormattedLine("libmpm", T_("%s: ignoring %s package"), packageInfo.deploymentName.c_str(), targetSystems.c_str());
      continue;
    }
#endif

#if POLLUTE_THE_DEBUG_STREAM
    trace_mpm->WriteFormattedLine("libmpm", T_("  adding %s"), packageInfo.deploymentName.c_str());
#endif

    count += 1;

    // insert into database
    PackageInfo* pPi = DefinePackage(packageInfo.deploymentName, packageInfo);

    // increment file ref counts, if package is installed
    if (pPi->timeInstalled > 0)
    {
      IncrementFileRefCounts(pPi->runFiles);
      IncrementFileRefCounts(pPi->docFiles);
      IncrementFileRefCounts(pPi->sourceFiles);
    }
  }

  trace_mpm->WriteFormattedLine("libmpm", T_("found %u package definition files"), static_cast<unsigned>(count));

  // determine dependencies
  for (auto& kv : packageTable)
  {
    PackageInfo& pkg = kv.second;
    // FIXME
    time_t timeInstalledMin = static_cast<time_t>(0xffffffffffffffffULL);
    time_t timeInstalledMax = 0;
    for (const string& req : pkg.requiredPackages)
    {
      PackageDefinitionTable::iterator it3 = packageTable.find(req);
      if (it3 == packageTable.end())
      {
        trace_mpm->WriteFormattedLine("libmpm", T_("dependancy problem: %s is required by %s"), req.c_str(), pkg.deploymentName.c_str());
      }
      else
      {
        it3->second.requiredBy.push_back(pkg.deploymentName);
        if (it3->second.timeInstalled < timeInstalledMin)
        {
          timeInstalledMin = it3->second.timeInstalled;
        }
        if (it3->second.timeInstalled > timeInstalledMax)
        {
          timeInstalledMax = it3->second.timeInstalled;
        }
      }
    }
    if (timeInstalledMin > 0)
    {
      if (pkg.IsPureContainer() || (pkg.IsInstalled() && pkg.timeInstalled < timeInstalledMax))
      {
        pkg.timeInstalled = timeInstalledMax;
      }
    }
  }

  // create "Obsolete" container
  PackageInfo piObsolete;
  piObsolete.deploymentName = "_miktex-obsolete";
  piObsolete.displayName = T_("Obsolete");
  piObsolete.title = T_("Obsolete packages");
  piObsolete.description = T_("Packages that were removed from the MiKTeX package repository.");
  for (auto& kv : packageTable)
  {
    PackageInfo& pkg = kv.second;
    if (!pkg.IsContained() && !pkg.IsContainer() && IsPackageObsolete(pkg.deploymentName))
    {
      piObsolete.requiredPackages.push_back(pkg.deploymentName);
      pkg.requiredBy.push_back(piObsolete.deploymentName);
    }
  }
  if (!piObsolete.requiredPackages.empty())
  {
    // insert "Obsolete" into the database
    DefinePackage(piObsolete.deploymentName, piObsolete);
  }

  // create "Uncategorized" container
  PackageInfo piOther;
  piOther.deploymentName = "_miktex-all-the-rest";
  piOther.displayName = T_("Uncategorized");
  piOther.title = T_("Uncategorized packages");
  for (auto& kv : packageTable)
  {
    PackageInfo& pkg = kv.second;
    if (!pkg.IsContained() && !pkg.IsContainer())
    {
      piOther.requiredPackages.push_back(pkg.deploymentName);
      pkg.requiredBy.push_back(piOther.deploymentName);
    }
  }
  if (!piOther.requiredPackages.empty())
  {
    // insert "Other" into the database
    DefinePackage(piOther.deploymentName, piOther);
  }
}

void PackageManagerImpl::ParseAllPackageDefinitionFiles()
{
  if (parsedAllPackageDefinitionFiles)
  {
    // we do this once
    return;
  }
  PathName userInstallRoot = session->GetSpecialPath(SpecialPath::UserInstallRoot);
  PathName commonInstallRoot = session->GetSpecialPath(SpecialPath::CommonInstallRoot);
  if (!session->IsAdminMode())
  {
    ParseAllPackageDefinitionFilesInDirectory(PathName(userInstallRoot, MIKTEX_PATH_PACKAGE_DEFINITION_DIR));
    if (userInstallRoot.Canonicalize() == commonInstallRoot.Canonicalize())
    {
      parsedAllPackageDefinitionFiles = true;
      return;
    }
  }
  ParseAllPackageDefinitionFilesInDirectory(PathName(commonInstallRoot, MIKTEX_PATH_PACKAGE_DEFINITION_DIR));
  parsedAllPackageDefinitionFiles = true;
}

void PackageManagerImpl::LoadDatabase(const PathName& path)
{
  // get the full path name
  PathName absPath(path);
  absPath.MakeAbsolute();

  // check to see whether it is an archive file or a directory
  bool isDirectory = Directory::Exists(absPath);

  unique_ptr<TemporaryDirectory> tempDir;

  PathName pathPackageInfoDir;

  if (isDirectory)
  {
    pathPackageInfoDir = absPath;
  }
  else
  {
    // create temporary directory
    tempDir = TemporaryDirectory::Create();

    pathPackageInfoDir = tempDir->GetPathName();

    // unpack the package definition files
    unique_ptr<MiKTeX::Extractor::Extractor> pExtractor(MiKTeX::Extractor::Extractor::CreateExtractor(DB_ARCHIVE_FILE_TYPE));
    pExtractor->Extract(absPath, pathPackageInfoDir);
  }

  // read package definition files
  ParseAllPackageDefinitionFilesInDirectory(pathPackageInfoDir);

  parsedAllPackageDefinitionFiles = true;
}

void PackageManagerImpl::ClearAll()
{
  packageTable.clear();
  installedFileInfoTable.clear();
  if (commonVariablePackageTable != nullptr)
  {
    commonVariablePackageTable = nullptr;
  }
  if (userVariablePackageTable != nullptr)
  {
    userVariablePackageTable = nullptr;
  }
  parsedAllPackageDefinitionFiles = false;
}

void PackageManagerImpl::UnloadDatabase()
{
  ClearAll();
}

PackageInfo* PackageManagerImpl::TryGetPackageInfo(const string& deploymentName)
{
  PackageDefinitionTable::iterator it = packageTable.find(deploymentName);
  if (it != packageTable.end())
  {
    return &it->second;
  }
  if (parsedAllPackageDefinitionFiles)
  {
    return nullptr;
  }
  PathName pathPackageDefinitionFile;
  bool havePackageInfoFile = false;
  if (!session->IsAdminMode())
  {
    pathPackageDefinitionFile = session->GetSpecialPath(SpecialPath::UserInstallRoot);
    pathPackageDefinitionFile /= MIKTEX_PATH_PACKAGE_DEFINITION_DIR;
    pathPackageDefinitionFile /= deploymentName;
    pathPackageDefinitionFile.AppendExtension(MIKTEX_PACKAGE_DEFINITION_FILE_SUFFIX);
    havePackageInfoFile = File::Exists(pathPackageDefinitionFile);
  }
  if (!havePackageInfoFile)
  {
    pathPackageDefinitionFile = session->GetSpecialPath(SpecialPath::CommonInstallRoot);
    pathPackageDefinitionFile /= MIKTEX_PATH_PACKAGE_DEFINITION_DIR;
    pathPackageDefinitionFile /= deploymentName;
    pathPackageDefinitionFile.AppendExtension(MIKTEX_PACKAGE_DEFINITION_FILE_SUFFIX);
    havePackageInfoFile = File::Exists(pathPackageDefinitionFile);
  }
  if (!havePackageInfoFile)
  {
    return nullptr;
  }
  unique_ptr<TpmParser> tpmParser = TpmParser::Create();
  tpmParser->Parse(pathPackageDefinitionFile);
#if IGNORE_OTHER_SYSTEMS
  string targetSystems = tpmParser->GetPackageInfo().targetSystem;
  if (targetSystems != "" && !StringUtil::Contains(targetSystems.c_str(), MIKTEX_SYSTEM_TAG))
  {
    return nullptr;
  }
#endif
  return DefinePackage(deploymentName, tpmParser->GetPackageInfo());
}

bool PackageManagerImpl::TryGetPackageInfo(const string& deploymentName, PackageInfo& packageInfo)
{
  PackageInfo* pPackageInfo = TryGetPackageInfo(deploymentName);
  if (pPackageInfo == nullptr)
  {
    return false;
  }
  else
  {
    packageInfo = *pPackageInfo;
    return true;
  }
}

PackageInfo PackageManagerImpl::GetPackageInfo(const string& deploymentName)
{
  const PackageInfo* pPackageInfo = TryGetPackageInfo(deploymentName);
  if (pPackageInfo == nullptr)
  {
    MIKTEX_FATAL_ERROR_2(T_("The requested package is unknown."), "name", deploymentName);
  }
  return *pPackageInfo;
}

unsigned long PackageManagerImpl::GetFileRefCount(const PathName& path)
{
  NeedInstalledFileInfoTable();
  InstalledFileInfoTable::const_iterator it = installedFileInfoTable.find(path.GetData());
  if (it == installedFileInfoTable.end())
  {
    return 0;
  }
  return it->second.refCount;
}

void PackageManagerImpl::NeedInstalledFileInfoTable()
{
  ParseAllPackageDefinitionFiles();
}

bool PackageManager::TryGetRemotePackageRepository(string& url, RepositoryReleaseState& repositoryReleaseState)
{
  shared_ptr<Session> session = Session::Get();
  repositoryReleaseState = RepositoryReleaseState::Unknown;
  if (session->TryGetConfigValue(MIKTEX_REGKEY_PACKAGE_MANAGER, MIKTEX_REGVAL_REMOTE_REPOSITORY, url))
  {
    string str;
    if (session->TryGetConfigValue(MIKTEX_REGKEY_PACKAGE_MANAGER, MIKTEX_REGVAL_REPOSITORY_RELEASE_STATE, str))
    {
      if (str == "stable")
      {
        repositoryReleaseState = RepositoryReleaseState::Stable;
      }
      else if (str == "next")
      {
        repositoryReleaseState = RepositoryReleaseState::Next;
      }
    }
    return true;
  }
  return Utils::GetEnvironmentString(MIKTEX_ENV_REPOSITORY, url)
    && (PackageRepositoryDataStore::DetermineRepositoryType(url) == RepositoryType::Remote);
}

string PackageManager::GetRemotePackageRepository(RepositoryReleaseState& repositoryReleaseState)
{
  string url;
  if (!TryGetRemotePackageRepository(url, repositoryReleaseState))
  {
    MIKTEX_UNEXPECTED();
  }
  return url;
}

void PackageManager::SetRemotePackageRepository(const string& url, RepositoryReleaseState repositoryReleaseState)
{
  shared_ptr<Session> session = Session::Get();
  session->SetConfigValue(MIKTEX_REGKEY_PACKAGE_MANAGER, MIKTEX_REGVAL_REMOTE_REPOSITORY, url);
  session->SetConfigValue(MIKTEX_REGKEY_PACKAGE_MANAGER, MIKTEX_REGVAL_REPOSITORY_RELEASE_STATE, repositoryReleaseState == RepositoryReleaseState::Stable ? "stable" : (repositoryReleaseState == RepositoryReleaseState::Next ? "next" : "unknown"));
}

bool PackageManager::TryGetLocalPackageRepository(PathName& path)
{
  shared_ptr<Session> session = Session::Get();
  string str;
  if (session->TryGetConfigValue(MIKTEX_REGKEY_PACKAGE_MANAGER, MIKTEX_REGVAL_LOCAL_REPOSITORY, str))
  {
    path = str;
    return true;
  }
  else if (Utils::GetEnvironmentString(MIKTEX_ENV_REPOSITORY, str) && (PackageRepositoryDataStore::DetermineRepositoryType(str) == RepositoryType::Local))
  {
    path = str;
    return true;
  }
  else
  {
    return false;
  }
}

PathName PackageManager::GetLocalPackageRepository()
{
  PathName path;
  if (!TryGetLocalPackageRepository(path))
  {
    MIKTEX_UNEXPECTED();
  }
  return path;
}

void PackageManager::SetLocalPackageRepository(const PathName& path)
{
  Session::Get()->SetConfigValue(MIKTEX_REGKEY_PACKAGE_MANAGER, MIKTEX_REGVAL_LOCAL_REPOSITORY, path.ToString());
}

bool PackageManager::TryGetMiKTeXDirectRoot(PathName& path)
{
  shared_ptr<Session> session = Session::Get();
  string str;
  if (session->TryGetConfigValue(MIKTEX_REGKEY_PACKAGE_MANAGER, MIKTEX_REGVAL_MIKTEXDIRECT_ROOT, str))
  {
    path = str;
    return true;
  }
  else if (Utils::GetEnvironmentString(MIKTEX_ENV_REPOSITORY, str) && (PackageRepositoryDataStore::DetermineRepositoryType(str) == RepositoryType::MiKTeXDirect))
  {
    path = str;
    return true;
  }
  else
  {
    return false;
  }
}

PathName PackageManager::GetMiKTeXDirectRoot()
{
  PathName path;
  if (!TryGetMiKTeXDirectRoot(path))
  {
    MIKTEX_UNEXPECTED();
  }
  return path;
}

void PackageManager::SetMiKTeXDirectRoot(const PathName& path)
{
  shared_ptr<Session> session = Session::Get();
  session->SetConfigValue(MIKTEX_REGKEY_PACKAGE_MANAGER, MIKTEX_REGVAL_MIKTEXDIRECT_ROOT, path.ToString());
}

RepositoryInfo PackageManager::GetDefaultPackageRepository()
{
  RepositoryInfo result;
  shared_ptr<Session> session = Session::Get();
  string str;
  if (session->TryGetConfigValue(MIKTEX_REGKEY_PACKAGE_MANAGER, MIKTEX_REGVAL_REPOSITORY_TYPE, str))
  {
    if (str == "remote")
    {
      result.url = GetRemotePackageRepository(result.releaseState);
      result.type = RepositoryType::Remote;
    }
    else if (str == "local")
    {
      result.url = GetLocalPackageRepository().ToString();
      result.type = RepositoryType::Local;
    }
    else if (str == "direct")
    {
      result.url = GetMiKTeXDirectRoot().ToString();
      result.type = RepositoryType::MiKTeXDirect;
    }
    else
    {
      MIKTEX_UNEXPECTED();
    }
  }
  else if (Utils::GetEnvironmentString(MIKTEX_ENV_REPOSITORY, result.url))
  {
    result.type = PackageRepositoryDataStore::DetermineRepositoryType(result.url);
  }
  else
  {
    result.url = "";
    result.type = RepositoryType::Remote;
  }
  return result;
}

bool PackageManager::TryGetDefaultPackageRepository(RepositoryType& repositoryType, RepositoryReleaseState& repositoryReleaseState, string& urlOrPath)
{
  RepositoryInfo defaultRepository = GetDefaultPackageRepository();
  repositoryType = defaultRepository.type;
  repositoryReleaseState = defaultRepository.releaseState;
  urlOrPath = defaultRepository.url;
  return true;
}

void PackageManager::SetDefaultPackageRepository(const RepositoryInfo& repository)
{
  shared_ptr<Session> session = Session::Get();
  string repositoryTypeStr;
  switch (repository.type)
  {
  case RepositoryType::MiKTeXDirect:
    repositoryTypeStr = "direct";
    SetMiKTeXDirectRoot(repository.url);
    break;
  case RepositoryType::Local:
    repositoryTypeStr = "local";
    SetLocalPackageRepository(repository.url);
    break;
  case RepositoryType::Remote:
    repositoryTypeStr = "remote";
    SetRemotePackageRepository(repository.url, repository.releaseState);
    break;
  default:
    MIKTEX_UNEXPECTED();
  }
  session->SetConfigValue(MIKTEX_REGKEY_PACKAGE_MANAGER, MIKTEX_REGVAL_REPOSITORY_TYPE, repositoryTypeStr);
}

void PackageManager::SetDefaultPackageRepository(RepositoryType repositoryType, RepositoryReleaseState repositoryReleaseState, const string& urlOrPath)
{
  shared_ptr<Session> session = Session::Get();
  RepositoryInfo repository;
  repository.type = repositoryType != RepositoryType::Unknown ? repositoryType : PackageRepositoryDataStore::DetermineRepositoryType(urlOrPath);
  repository.releaseState = repositoryReleaseState;
  repository.url = urlOrPath;
  SetDefaultPackageRepository(repository);
}

namespace {

  typedef unordered_set<string, hash_path, equal_path> SubDirectoryTable;

  struct DirectoryInfo
  {
    SubDirectoryTable subDirectoryNames;
    vector<string> fileNames;
    vector<string> packageNames;
  };


  typedef unordered_map<string, DirectoryInfo, hash_path, equal_path> DirectoryInfoTable;

  // directory info table: written by
  // PackageManagerImpl::CreateMpmFndb(); read by
  // PackageManagerImpl::ReadDirectory()
  DirectoryInfoTable directoryInfoTable;

}

MPMSTATICFUNC(void) RememberFileNameInfo(const string& prefixedFileName, const string& packageName)
{
  shared_ptr<Session> session = Session::Get();

  string fileName;

  // ignore non-texmf files
  if (!PackageManager::StripTeXMFPrefix(prefixedFileName, fileName))
  {
    return;
  }

  PathNameParser pathtok(fileName);

  if (!pathtok)
  {
    return;
  }

  // initialize root path: "//MiKTeX/[MPM]"
  PathName path = session->GetMpmRootPath();
  //  path += CURRENT_DIRECTORY;

  // s1: current path name component
  string s1 = *pathtok;
  ++pathtok;

  // name: file name component
  string name = s1;

  while (pathtok)
  {
    string s2 = *pathtok;
    ++pathtok;
    directoryInfoTable[path.GetData()].subDirectoryNames.insert(s1);
    name = s2;
#if defined(MIKTEX_WINDOWS)
    // make sure the the rest of the path contains slashes (not
    // backslashes)
    path.AppendAltDirectoryDelimiter();
#else
    path.AppendDirectoryDelimiter();
#endif
    path /= s1;
    s1 = s2;
  }

  DirectoryInfo& directoryInfo = directoryInfoTable[path.ToString()];
  directoryInfo.fileNames.push_back(name);
  directoryInfo.packageNames.push_back(packageName);
}

bool PackageManagerImpl::ReadDirectory(const PathName& path, vector<string>& subDirNames, vector<string>& fileNames, vector<string>& fileNameInfos)
{
  const DirectoryInfo& directoryInfo = directoryInfoTable[path.ToString()];
  for (const string& name : directoryInfo.subDirectoryNames)
  {
    subDirNames.push_back(name);
  }
  fileNames = directoryInfo.fileNames;
  fileNameInfos = directoryInfo.packageNames;
  return true;
}

bool PackageManagerImpl::OnProgress(unsigned level, const PathName& directory)
{
  UNUSED_ALWAYS(level);
  UNUSED_ALWAYS(directory);
  return true;
}

void PackageManagerImpl::CreateMpmFndb()
{
  ParseAllPackageDefinitionFiles();

  // collect the file names
  for (const auto& kv : packageTable)
  {
    const PackageInfo& pi = kv.second;
    for (const string& file : pi.runFiles)
    {
      RememberFileNameInfo(file, pi.deploymentName);
    }
    for (const string& file : pi.docFiles)
    {
      RememberFileNameInfo(file, pi.deploymentName);
    }
    for (const string& file : pi.sourceFiles)
    {
      RememberFileNameInfo(file, pi.deploymentName);
    }
  }

  // create the database
  Fndb::Create(session->GetMpmDatabasePathName().GetData(), session->GetMpmRootPath().GetData(), this, true, true);

  // free memory
  directoryInfoTable.clear();
}

void PackageManagerImpl::GetAllPackageDefinitions(vector<PackageInfo>& packages)
{
  ParseAllPackageDefinitionFiles();
  for (const auto& kv : packageTable)
  {
    packages.push_back(kv.second);
  }
}

InstalledFileInfo* PackageManagerImpl::GetInstalledFileInfo(const char* lpszPath)
{
  ParseAllPackageDefinitionFiles();
  InstalledFileInfoTable::iterator it = installedFileInfoTable.find(lpszPath);
  if (it == installedFileInfoTable.end())
  {
    return nullptr;
  }
  return &it->second;
}

bool PackageManager::IsLocalPackageRepository(const PathName& path)
{
  if (!Directory::Exists(path))
  {
    return false;
  }

  // local mirror of remote package repository?
  if (File::Exists(PathName(path, MIKTEX_MPM_DB_LIGHT_FILE_NAME))
    && File::Exists(PathName(path, MIKTEX_MPM_DB_FULL_FILE_NAME)))
  {
    return true;
  }

  return false;
}

PackageInfo PackageManager::ReadPackageDefinitionFile(const PathName& path, const string& texmfPrefix)
{
  unique_ptr<TpmParser> tpmParser = TpmParser::Create();
  tpmParser->Parse(path, texmfPrefix);
  return tpmParser->GetPackageInfo();
}

class XmlWriter
{
public:
  XmlWriter(const PathName& path) :
    stream(File::CreateOutputStream(path))
  {
    stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" << "\n";
  }

public:
  void Close()
  {
    stream.close();
  }

public:
  void StartElement(const string& name)
  {
    if (freshElement)
    {
      stream << '>';
    }
    stream << fmt::format("<{}", name);
    freshElement = true;
    elements.push(name);
  }

public:
  void AddAttribute(const string& name, const string& value)
  {
    stream << fmt::format(" {}=\"{}\"", name, value);
  }

public:
  void EndElement()
  {
    if (elements.empty())
    {
      MIKTEX_UNEXPECTED();
    }
    if (freshElement)
    {
      stream << "/>";
      freshElement = false;
    }
    else
    {
      stream << fmt::format("</{}>", elements.top());
    }
    elements.pop();
  }

public:
  void EndAllElements()
  {
    while (!elements.empty())
    {
      EndElement();
    }
  }

public:
  void Text(const string& text)
  {
    if (freshElement)
    {
      stream << '>';
      freshElement = false;
    }
    for (const char& ch : text)
    {
      switch (ch)
      {
      case '&':
        stream << "&amp;";
        break;
      case '<':
        stream << "&lt;";
        break;
      case '>':
        stream << "&gt;";
        break;
      default:
        stream << ch;
        break;
      }
    }
  }

private:
  ofstream stream;

private:
  stack<string> elements;

private:
  bool freshElement = false;
};

void PackageManager::WritePackageDefinitionFile(const PathName& path, const PackageInfo& packageInfo, time_t timePackaged)
{
  XmlWriter xml(path);

  // create "rdf:Description" node
  xml.StartElement("rdf:RDF");
  xml.AddAttribute("xmlns:rdf", "http://www.w3.org/1999/02/22-rdf-syntax-ns#");
  xml.AddAttribute("xmlns:TPM", "http://texlive.dante.de/");
  xml.StartElement("rdf:Description");
  string about("http://www.miktex.org/packages/");
  about += packageInfo.deploymentName;
  xml.AddAttribute("about", about.c_str());

  // create "TPM:Name" node
  xml.StartElement("TPM:Name");
  xml.Text(packageInfo.displayName);
  xml.EndElement();

  // create "TPM:Creator" node
  xml.StartElement("TPM:Creator");
  xml.Text("mpc");
  xml.EndElement();

  // create "TPM:Title" node
  xml.StartElement("TPM:Title");
  xml.Text(packageInfo.title);
  xml.EndElement();

  // create "TPM:Version" node
  xml.StartElement("TPM:Version");
  xml.Text(packageInfo.version);
  xml.EndElement();

  // create "TPM:TargetSystem" node
  xml.StartElement("TPM:TargetSystem");
  xml.Text(packageInfo.targetSystem);
  xml.EndElement();

  // create "TPM:Description" node
  xml.StartElement("TPM:Description");
  xml.Text(packageInfo.description);
  xml.EndElement();


  // create "TPM:RunFiles" node
  if (!packageInfo.runFiles.empty())
  {
    xml.StartElement("TPM:RunFiles");
    xml.AddAttribute("size", std::to_string(static_cast<unsigned>(packageInfo.sizeRunFiles)).c_str());
    bool start = true;
    for (const string& file : packageInfo.runFiles)
    {
      if (start)
      {
        start = false;
      }
      else
      {
        xml.Text(" ");
      }      
      xml.Text(file);
    }
    xml.EndElement();
  }

  // create "TPM:DocFiles" node
  if (!packageInfo.docFiles.empty())
  {
    xml.StartElement("TPM:DocFiles");
    xml.AddAttribute("size", std::to_string(static_cast<unsigned>(packageInfo.sizeDocFiles)).c_str());
    bool start = true;
    for (const string& file : packageInfo.docFiles)
    {
      if (start)
      {
        start = false;
      }
      else
      {
        xml.Text(" ");
      }      
      xml.Text(file);
    }
    xml.EndElement();
  }

  // create "TPM:SourceFiles" node
  if (!packageInfo.sourceFiles.empty())
  {
    xml.StartElement("TPM:SourceFiles");
    xml.AddAttribute("size", std::to_string(static_cast<unsigned>(packageInfo.sizeSourceFiles)).c_str());
    bool start = true;
    for (const string& file : packageInfo.sourceFiles)
    {
      if (start)
      {
        start = false;
      }
      else
      {
        xml.Text(" ");
      }      
      xml.Text(file);
    }
    xml.EndElement();
  }

  // create "TPM:Requires" node
  if (!packageInfo.requiredPackages.empty())
  {
    xml.StartElement("TPM:Requires");
    for (const string& req : packageInfo.requiredPackages)
    {
      xml.StartElement("TPM:Package");
      xml.AddAttribute("name", req.c_str());
      xml.EndElement();
    }
    xml.EndElement();
  }

  // create "TPM:TimePackaged" node
  if (timePackaged != 0)
  {
    xml.StartElement("TPM:TimePackaged");
    xml.Text(std::to_string(timePackaged));
    xml.EndElement();
  }

  // create "TPM:MD5" node
  xml.StartElement("TPM:MD5");
  xml.Text(packageInfo.digest.ToString());
  xml.EndElement();

#if MIKTEX_EXTENDED_PACKAGEINFO
  if (!packageInfo.ctanPath.empty())
  {
    xml.StartElement("TPM:CTAN");
    xml.AddAttribute("path", packageInfo.ctanPath.c_str());
    xml.EndElement();
  }

  if (!(packageInfo.copyrightOwner.empty() && packageInfo.copyrightYear.empty()))
  {
    xml.StartElement("TPM:Copyright");
    xml.AddAttribute("owner", packageInfo.copyrightOwner.c_str());
    xml.AddAttribute("year", packageInfo.copyrightYear.c_str());
    xml.EndElement();
  }

  if (!packageInfo.licenseType.empty())
  {
    xml.StartElement("TPM:License");
    xml.AddAttribute("type", packageInfo.licenseType.c_str());
    xml.EndElement();
  }
#endif

  xml.EndAllElements();

  xml.Close();
}

bool PackageManager::StripTeXMFPrefix(const string& str, string& result)
{
  if (StripPrefix(str, TEXMF_PREFIX_DIRECTORY, result))
  {
    return true;
  }
  PathName prefix2(".");
  prefix2 /= TEXMF_PREFIX_DIRECTORY;
  return StripPrefix(str, prefix2.GetData(), result);
}

void PackageManager::SetProxy(const ProxySettings& proxySettings)
{
  shared_ptr<Session> session = Session::Get();
  session->SetConfigValue(MIKTEX_REGKEY_PACKAGE_MANAGER, MIKTEX_REGVAL_USE_PROXY, proxySettings.useProxy);
  session->SetConfigValue(MIKTEX_REGKEY_PACKAGE_MANAGER, MIKTEX_REGVAL_PROXY_HOST, proxySettings.proxy);
  session->SetConfigValue(MIKTEX_REGKEY_PACKAGE_MANAGER, MIKTEX_REGVAL_PROXY_PORT, proxySettings.port);
  session->SetConfigValue(MIKTEX_REGKEY_PACKAGE_MANAGER, MIKTEX_REGVAL_PROXY_AUTH_REQ, proxySettings.authenticationRequired);
  PackageManagerImpl::proxyUser = proxySettings.user;
  PackageManagerImpl::proxyPassword = proxySettings.password;
}

bool PackageManager::TryGetProxy(const string& url, ProxySettings& proxySettings)
{
  shared_ptr<Session> session = Session::Get(); 
  proxySettings.useProxy = session->GetConfigValue(MIKTEX_REGKEY_PACKAGE_MANAGER, MIKTEX_REGVAL_USE_PROXY, false).GetBool();
  if (proxySettings.useProxy)
  {
    if (!session->TryGetConfigValue(MIKTEX_REGKEY_PACKAGE_MANAGER, MIKTEX_REGVAL_PROXY_HOST, proxySettings.proxy))
    {
      return false;
    }
    proxySettings.port = session->GetConfigValue(MIKTEX_REGKEY_PACKAGE_MANAGER, MIKTEX_REGVAL_PROXY_PORT, 8080).GetInt();
    proxySettings.authenticationRequired = session->GetConfigValue(MIKTEX_REGKEY_PACKAGE_MANAGER, MIKTEX_REGVAL_PROXY_AUTH_REQ, false).GetBool();
    proxySettings.user = PackageManagerImpl::proxyUser;
    proxySettings.password = PackageManagerImpl::proxyPassword;
    return true;
  }
  string proxyEnv;
  if (!url.empty())
  {
    Uri uri(url.c_str());
    string scheme = uri.GetScheme();
    string envName;
    if (scheme == "https")
    {
      envName = "https_proxy";
    }
    else if (scheme == "http")
    {
      envName = "http_proxy";
    }
    else if (scheme == "ftp")
    {
      envName = "FTP_PROXY";
    }
    else
    {
      MIKTEX_UNEXPECTED();
    }
    Utils::GetEnvironmentString(envName, proxyEnv);
  }
  if (proxyEnv.empty())
  {
    Utils::GetEnvironmentString("ALL_PROXY", proxyEnv);
  }
  if (proxyEnv.empty())
  {
    return false;
  }
  Uri uri(proxyEnv);
  proxySettings.useProxy = true;
  proxySettings.proxy = uri.GetHost();
  proxySettings.port = uri.GetPort();
  string userInfo = uri.GetUserInfo();
  proxySettings.authenticationRequired = !userInfo.empty();
  if (proxySettings.authenticationRequired)
  {
    string::size_type idx = userInfo.find_first_of(":");
    if (idx == string::npos)
    {
      proxySettings.user = userInfo;
      proxySettings.password = "";
    }
    else
    {
      proxySettings.user = userInfo.substr(0, idx);
      proxySettings.password = userInfo.substr(idx + 1);
    }
  }
  else
  {
    proxySettings.user = "";
    proxySettings.password = "";
  }
  return true;
}

bool PackageManager::TryGetProxy(ProxySettings& proxySettings)
{
  return TryGetProxy("", proxySettings);
}

ProxySettings PackageManager::GetProxy(const string& url)
{
  ProxySettings proxySettings;
  if (!TryGetProxy(url, proxySettings))
  {
    MIKTEX_FATAL_ERROR(T_("No proxy host is configured."));
  }
  return proxySettings;
}

ProxySettings PackageManager::GetProxy()
{
  return GetProxy("");
}

void PackageManagerImpl::OnProgress()
{
}

bool PackageManagerImpl::TryGetFileDigest(const PathName& prefix, const string& fileName, bool& haveDigest, MD5& digest)
{
  string unprefixed;
  if (!StripTeXMFPrefix(fileName, unprefixed))
  {
    return true;
  }
  PathName path = prefix;
  path /= unprefixed;
  if (!File::Exists(path))
  {
    trace_mpm->WriteFormattedLine("libmpm", T_("package verification failed: file %s does not exist"), Q_(path));
    return false;
  }
  if (path.HasExtension(MIKTEX_PACKAGE_DEFINITION_FILE_SUFFIX))
  {
    haveDigest = false;
  }
  else
  {
    digest = MD5::FromFile(path);
    haveDigest = true;
  }
  return true;
}

bool PackageManagerImpl::TryCollectFileDigests(const PathName& prefix, const vector<string>& files, FileDigestTable& fileDigests)
{
  for (const string& fileName : files)
  {
    bool haveDigest;
    MD5 digest;
    if (!TryGetFileDigest(prefix, fileName, haveDigest, digest))
    {
      return false;
    }
    if (haveDigest)
    {
      fileDigests[fileName] = digest;
    }
  }
  return true;
}

bool PackageManagerImpl::TryVerifyInstalledPackage(const string& deploymentName)
{
  PackageInfo packageInfo = GetPackageInfo(deploymentName);

  PathName prefix;

  if (!session->IsAdminMode() && GetUserTimeInstalled(deploymentName) != static_cast<time_t>(0))
  {
    prefix = session->GetSpecialPath(SpecialPath::UserInstallRoot);
  }

  if (prefix.Empty())
  {
    prefix = session->GetSpecialPath(SpecialPath::CommonInstallRoot);
  }

  FileDigestTable fileDigests;

  if (!TryCollectFileDigests(prefix, packageInfo.runFiles, fileDigests)
    || !TryCollectFileDigests(prefix, packageInfo.docFiles, fileDigests)
    || !TryCollectFileDigests(prefix, packageInfo.sourceFiles, fileDigests))
  {
    return false;
  }

  MD5Builder md5Builder;

  for (const pair<string, MD5> p : fileDigests)
  {
    PathName path(p.first);
    // we must dosify the path name for backward compatibility
    path.ConvertToDos();
    md5Builder.Update(path.GetData(), path.GetLength());
    md5Builder.Update(p.second.data(), p.second.size());
  }

  bool ok = md5Builder.Final() == packageInfo.digest;

  if (!ok)
  {
    trace_mpm->WriteFormattedLine("libmpm", T_("package %s verification failed: some files have been modified"), Q_(deploymentName));
    trace_mpm->WriteFormattedLine("libmpm", T_("expected digest: %s"), packageInfo.digest.ToString().c_str());
    trace_mpm->WriteFormattedLine("libmpm", T_("computed digest: %s"), md5Builder.GetMD5().ToString().c_str());
  }

  return ok;
}

string PackageManagerImpl::GetContainerPath(const string& deploymentName, bool useDisplayNames)
{
  string path;
  PackageInfo packageInfo = GetPackageInfo(deploymentName);
  for (const string& reqby : packageInfo.requiredBy)
  {
    PackageInfo packageInfo2 = GetPackageInfo(reqby);
    if (packageInfo2.IsPureContainer())
    {
      // RECUSION
      path = GetContainerPath(reqby, useDisplayNames);
      path += PathName::DirectoryDelimiter;
      if (useDisplayNames)
      {
        path += packageInfo2.displayName;
      }
      else
      {
        path += packageInfo2.deploymentName;
      }
      break;
    }
  }
  return path;
}

BEGIN_INTERNAL_NAMESPACE;

bool IsUrl(const string& url)
{
  string::size_type pos = url.find("://");
  if (pos == string::npos)
  {
    return false;
  }
  string scheme = url.substr(0, pos);
  for (const char& ch : scheme)
  {
    if (!isalpha(ch, locale()))
    {
      return false;
    }
  }
  return true;
}

string MakeUrl(const string& base, const string& rel)
{
  string url(base);
  size_t l = url.length();
  if (l == 0)
  {
    MIKTEX_UNEXPECTED();
  }
  if (url[l - 1] != '/')
  {
    url += '/';
  }
  if (rel[0] == '/')
  {
    MIKTEX_UNEXPECTED();
  }
  url += rel;
  return url;
}

END_INTERNAL_NAMESPACE;