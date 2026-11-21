param([Parameter(Mandatory)][string]$OutputDirectory)

# A private Excel instance and a generated workbook. Exporting to XPS keeps
# the user's clipboard and any open workbooks out of the measurement.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName ReachFramework, PresentationCore, PresentationFramework, WindowsBase
$folder = (Get-Item -LiteralPath $OutputDirectory).FullName
$xpsPath = Join-Path $folder 'office-sheet.xps'
$bitmapPath = Join-Path $folder 'office-sheet.bgra'
if ((Test-Path -LiteralPath $xpsPath) -or (Test-Path -LiteralPath $bitmapPath)) {
    throw 'Use a new output directory.'
}
$excel = New-Object -ComObject Excel.Application
try {
    $excel.DisplayAlerts = $false
    $book = $excel.Workbooks.Add()
    $sheet = $book.Worksheets.Item(1)
    $range = $sheet.Range('A1:G56')
    $range.Font.Name = 'Malgun Gothic'
    $range.Font.Size = 9
    $range.ColumnWidth = 15
    $range.RowHeight = 15
    $range.Borders.LineStyle = 1
    $range.Borders.Weight = 2
    $range.Borders.Color = 0xcccccc
    # Korean text is built from Unicode code points so Windows PowerShell 5
    # can run the checked-in UTF-8 file without relying on its ANSI code page.
    $item = -join ([char[]](0xc815,0xc0b0,0xb0b4,0xc5ed))
    $amount = -join ([char[]](0xae08,0xc561))
    $values = New-Object 'object[,]' 56,7
    for ($row = 0; $row -lt 56; $row++) {
        for ($col = 0; $col -lt 7; $col++) {
            $values[$row,$col] = if ($col % 3 -eq 0) { "$item $($row+1)" }
                elseif ($col % 3 -eq 1) { "$amount $col" }
                else { [double](($row + 1) * 13.57 + $col) }
        }
    }
    $range.Value2 = $values
    $sheet.Range('C1:C56').NumberFormat = '#,##0.00'
    $sheet.Range('C1:C56').Font.Color = 0xa05020
    $sheet.Range('F1:F56').Font.Color = 0x2020aa
    $page = $sheet.PageSetup
    $page.PaperSize = 9 # A4 at 96 dpi, wide enough for the comparison area.
    $page.LeftMargin = 0
    $page.TopMargin = 0
    $page.RightMargin = 0
    $page.BottomMargin = 0
    $page.PrintArea = $range.Address()
    $page.Zoom = 100
    $sheet.ExportAsFixedFormat(1, $xpsPath)
} finally {
    if ($book) { $book.Close($false) }
    $excel.Quit()
}

$doc = New-Object System.Windows.Xps.Packaging.XpsDocument($xpsPath,[IO.FileAccess]::Read)
try {
    $page = $doc.GetFixedDocumentSequence().DocumentPaginator.GetPage(0)
    $bitmap = New-Object System.Windows.Media.Imaging.RenderTargetBitmap(800,900,96,96,[Windows.Media.PixelFormats]::Pbgra32)
    $visual = New-Object System.Windows.Media.DrawingVisual
    $draw = $visual.RenderOpen()
    $draw.DrawRectangle([Windows.Media.Brushes]::White,$null,(New-Object Windows.Rect(0,0,800,900)))
    $draw.Close()
    $bitmap.Render($visual)
    $bitmap.Render($page.Visual)
    $pixels = New-Object byte[] (800 * 900 * 4)
    $bitmap.CopyPixels($pixels, 800 * 4, 0)
    [IO.File]::WriteAllBytes($bitmapPath, $pixels)
    Write-Output "Excel sheet exported to $bitmapPath (800 x 900 BGRA)."
} finally { $doc.Close() }
