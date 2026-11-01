// Port of WinSCP source/core/Option.{h,cpp} (TOptions) and the command line
// tokenizer CutToken from source/core/Common.cpp.
// Used both for the program command line ("/switch" or "-switch") and for
// parameters of script commands ("-switch" only).
#pragma once

#include <QString>
#include <QStringList>
#include <vector>

// Cuts the first token off Str; tokens are separated by spaces/tabs,
// double quotes group, "" inside is a literal quote (DoCutToken)
bool CutToken(QString & Str, QString & Token, QString * RawToken = nullptr, QString * Separator = nullptr);
// Quotes a parameter for a command line when needed (AddQuotes + EscapeParam)
QString QuoteParam(const QString & Param);

class Options
{
public:
  Options();

  void SetSwitchMarks(const QString & Marks) { FSwitchMarks = Marks; }
  void Parse(const QString & CmdLine);
  void Add(const QString & Value);

  int ParamCount() const { return FParamCount; }
  // 1-based, as in WinSCP
  QString Param(int Index);
  bool Empty() const { return FOptions.empty(); }

  bool FindSwitch(const QString & Switch);
  bool FindSwitch(const QString & Switch, QString & Value);
  bool FindSwitch(const QString & Switch, QString & Value, bool & ValueSet);
  // Switch followed by a list of parameters ("/command a b c");
  // the parameters are consumed (removed from the parameter list)
  bool FindSwitch(const QString & Switch, QStringList & Params, int ParamsMax = -1);
  QString SwitchValue(const QString & Switch, const QString & Default);
  // "on"/"off"/number; throws ExtException on an invalid value
  bool SwitchValue(const QString & Switch, bool Default, bool DefaultOnNonExistence);
  bool SwitchValue(const QString & Switch, bool Default) { return SwitchValue(Switch, Default, Default); }
  bool UnusedSwitch(QString & Switch) const;
  // Last added option was a switch (for password masking)
  bool WasSwitchAdded(QString & Switch, QString & Value, QChar & SwitchMark) const;

  void ParamsProcessed(int ParamsStart, int ParamsCount);

private:
  enum OptionType { otParam, otSwitch };
  struct Option
  {
    OptionType Type = otParam;
    QString Name;
    QString Value;
    bool ValueSet = false;
    bool Used = false;
    QChar SwitchMark;
  };

  QString FSwitchMarks;
  QString FSwitchValueDelimiters;
  std::vector<Option> FOptions;
  bool FNoMoreSwitches = false;
  int FParamCount = 0;

  bool DoFindSwitch(const QString & Switch, QString & Value, int & ParamsStart, int & ParamsCount, bool & ValueSet);
};
