On Error Resume Next
Set o = CreateObject("DspackPreview.PreviewHandler")
If Err.Number = 0 Then
    WScript.Echo "NATIVE CREATE OK: " & TypeName(o)
    Set o = Nothing
Else
    WScript.Echo "NATIVE CREATE FAIL: 0x" & Hex(Err.Number) & " " & Err.Description
End If
WScript.Quit 0