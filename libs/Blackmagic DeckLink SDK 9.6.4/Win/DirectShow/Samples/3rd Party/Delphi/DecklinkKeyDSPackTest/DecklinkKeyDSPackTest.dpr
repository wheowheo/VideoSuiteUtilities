program DecklinkKeyDSPackTest;

uses
  Forms,
  TfrmMainUnit in 'TfrmMainUnit.pas' {frmMain};

{$R *.res}

begin
  Application.Initialize;
  Application.CreateForm(TfrmMain, frmMain);
  Application.Run;
end.
