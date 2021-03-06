/* c4pstart.cpp: C4P statup code

   Copyright (C) 1996-2018 Christian Schenk

   This file is part of the MiKTeX TeXMF Library.

   The MiKTeX TeXMF Library is free software; you can redistribute it
   and/or modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2, or
   (at your option) any later version.

   The MiKTeX TeXMF Library is distributed in the hope that it will be
   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with the MiKTeX TeXMF Library; if not, write to the Free
   Software Foundation, 59 Temple Place - Suite 330, Boston, MA
   02111-1307, USA. */

#include "internal.h"

struct ProgramData
{
  ~ProgramData()
  {
    ClearCommandLine();
  }
  void ClearCommandLine()
  {
    for (char* lpsz : argumentVector)
    {
      free(lpsz);
    }
    argumentVector.clear();
    commandLine = "";
  }
  time_t startUpTime = static_cast<time_t>(-1);
  struct tm startUpTimeStructLocal;
  struct tm startUpTimeStructUtc;
  bool startUpTimeUseUtc;
  C4P_text standardTextFiles[3];
  vector<char*> argumentVector;
  string commandLine;
  string programName;
};

namespace {
  ProgramData programData;
}

C4PCEEAPI(int) C4P::MakeCommandLine(const vector<string>& args)
{
  MIKTEX_API_BEGIN("C4P::MakeCommandLine");
  programData.ClearCommandLine();
  programData.argumentVector.push_back(strdup(Utils::GetExeName().c_str()));
  for (const string& arg : args)
  {
    programData.argumentVector.push_back(strdup(arg.c_str()));
    programData.commandLine += ' ';
    programData.commandLine += arg;
  }
  return 0;
  MIKTEX_API_END("C4P::MakeCommandLine");
}

C4PCEEAPI(void) C4P::SetStartUpTime(time_t time, bool useUtc)
{
  MIKTEX_API_BEGIN("C4P::SetStartUpTime");
  programData.startUpTime = time;
  programData.startUpTimeUseUtc = useUtc;
#if defined(_MSC_VER) && (_MSC_VER) >= 1400
  if (_localtime64_s(&programData.startUpTimeStructLocal, &programData.startUpTime) != 0)
  {
    MIKTEX_FATAL_CRT_ERROR("_localtime_s");
  }
  if (_gmtime64_s(&programData.startUpTimeStructUtc, &programData.startUpTime) != 0)
  {
    MIKTEX_FATAL_CRT_ERROR("_gmtime64_s");
  }
#else
  struct tm* localTm = localtime(&programData.startUpTime);
  if (localTm == nullptr)
  {
    MIKTEX_FATAL_CRT_ERROR("localtime");
  }
  programData.startUpTimeStructLocal = *localTm;
  struct tm* utcTm = gmtime(&programData.startUpTime);
  if (utcTm == nullptr)
  {
    MIKTEX_FATAL_CRT_ERROR("gmtime");
  }
  programData.startUpTimeStructUtc = *utcTm;
#endif
  MIKTEX_API_END("C4P::SetStartUpTime");
}

C4PCEEAPI(time_t) C4P::GetStartUpTime()
{
  MIKTEX_API_BEGIN("C4P::GetStartUpTime");
  return programData.startUpTime;
  MIKTEX_API_END("C4P::GetStartUpTime");
}

class C4P::Program::impl
{
public:
  bool isRunning = false;
};

C4P::Program::Program(const char* programName, int argc, char* argv[]) :
  pimpl(make_unique<impl>())
{
  MIKTEX_API_BEGIN("C4P::StartUp");
  MIKTEX_ASSERT_STRING(programName);
  programData.programName = programName;
  if (programData.startUpTime == static_cast<time_t>(-1))
  {
    string sde;
    string fsd;
    time_t now;
    bool useUtc;
    if (Utils::GetEnvironmentString("FORCE_SOURCE_DATE", fsd) && fsd == "1" && Utils::GetEnvironmentString("SOURCE_DATE_EPOCH", sde))
    {
      now = Utils::ToTimeT(sde);
      useUtc = true;
    }
    else
    {
      now = time(nullptr);
      useUtc = false;
    }
    SetStartUpTime(now, useUtc);
  }
  vector<string> args;
  for (int idx = 1; idx < argc; ++idx)
  {
    args.push_back(argv[idx]);
  }
  MakeCommandLine(args);
  programData.standardTextFiles[0].Attach(stdin, false);
  programData.standardTextFiles[1].Attach(stdout, false);
  programData.standardTextFiles[2].Attach(stderr, false);
  *input = '\n';
  *output = '\0';
  *c4perroroutput = '\0';
  pimpl->isRunning = true;
  MIKTEX_API_END("C4P::StartUp");
}

C4P::Program::~Program() noexcept
{
  try
  {
    if (pimpl->isRunning)
    {
      Finish();
    }
  }
  catch (const std::exception&)
  {
  }
}

C4PTHISAPI(void) C4P::Program::Finish()
{
  programData.ClearCommandLine();
  programData.programName = "";
}

C4PCEEAPI(unsigned) C4P::GetYear()
{
  MIKTEX_API_BEGIN("C4P::GetYear");
  const tm& tmData = programData.startUpTimeUseUtc ? programData.startUpTimeStructUtc : programData.startUpTimeStructLocal;
  return tmData.tm_year + 1900;
  MIKTEX_API_END("C4P::GetYear");
}

C4PCEEAPI(unsigned) C4P::GetMonth()
{
  MIKTEX_API_BEGIN("C4P::GetMonth");
  const tm& tmData = programData.startUpTimeUseUtc ? programData.startUpTimeStructUtc : programData.startUpTimeStructLocal;
  return tmData.tm_mon + 1;
  MIKTEX_API_END("C4P::GetMonth");
}

C4PCEEAPI(unsigned) C4P::GetDay()
{
  MIKTEX_API_BEGIN("C4P::GetDay");
  const tm& tmData = programData.startUpTimeUseUtc ? programData.startUpTimeStructUtc : programData.startUpTimeStructLocal;
  return tmData.tm_mday;
  MIKTEX_API_END("C4P::GetDay");
}

C4PCEEAPI(unsigned) C4P::GetHour()
{
  MIKTEX_API_BEGIN("C4P::GetHour");
  const tm& tmData = programData.startUpTimeUseUtc ? programData.startUpTimeStructUtc : programData.startUpTimeStructLocal;
  return tmData.tm_hour;
  MIKTEX_API_END("C4P::GetHour");
}

C4PCEEAPI(unsigned) C4P::GetMinute()
{
  MIKTEX_API_BEGIN("C4P::GetMinute");
  const tm& tmData = programData.startUpTimeUseUtc ? programData.startUpTimeStructUtc : programData.startUpTimeStructLocal;
  return tmData.tm_min;
  MIKTEX_API_END("C4P::GetMinute");
}

C4PCEEAPI(unsigned) C4P::GetSecond()
{
  MIKTEX_API_BEGIN("C4P::GetSecond");
  const tm& tmData = programData.startUpTimeUseUtc ? programData.startUpTimeStructUtc : programData.startUpTimeStructLocal;
  return tmData.tm_sec;
  MIKTEX_API_END("C4P::GetSecond");
}

C4PCEEAPI(C4P::C4P_text *) C4P::GetStdFilePtr(unsigned idx)
{
  MIKTEX_API_BEGIN("C4P::GetStdFilePtr");
  MIKTEX_ASSERT(idx < sizeof(programData.standardTextFiles) / sizeof(programData.standardTextFiles[0]));
  return &programData.standardTextFiles[idx];
  MIKTEX_API_END("C4P::GetStdFilePtr");
}

C4PCEEAPI(int) C4P::GetArgC()
{
  MIKTEX_API_BEGIN("C4P::GetArgC");
  return static_cast<int>(programData.argumentVector.size());
  MIKTEX_API_END("C4P::GetArgC");
}

C4PCEEAPI(const char**) C4P::GetArgV()
{
  MIKTEX_API_BEGIN("C4P::GetArgV");
  return const_cast<const char**>(&programData.argumentVector[0]);
  MIKTEX_API_END("C4P::GetArgV");
}

C4PCEEAPI(const char*) C4P::GetCmdLine()
{
  MIKTEX_API_BEGIN("C4P::GetCmdLine");
  return programData.commandLine.c_str();
  MIKTEX_API_END("C4P::GetCmdLine");
}
