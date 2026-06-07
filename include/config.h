#ifndef CONFIG_H
#define CONFIG_H

struct Config {
  bool EnableDumpGraph;
  bool DumpGraphDot;
  bool DumpGraphJson;
  bool DumpAssignTree;
  bool DumpConstStatus;
  bool DumpMtScheduleJson;
  std::string MtHelperMode;
  std::string OutputDir;
  std::string InputBaseName;
  int SuperNodeMaxSize;
  uint32_t cppMaxSizeKB;
  std::string sep_module;
  std::string sep_aggr;
  int MergeWhenSize;
  int When2muxBound;
  int LogLevel;
  std::set<std::string> DumpStages;
  Config();
};

extern Config globalConfig;

#endif
