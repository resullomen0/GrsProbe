# GrsProbe - WSL ile Windows CE ARM derleme scripti
# PowerShell ile çalıştır: powershell -ExecutionPolicy Bypass -File DERLEME_WSL.ps1
#
# Ön koşul: WSL (Windows Subsystem for Linux) aktif olmalı
# WSL yoksa: Start Menu -> "Windows özelliklerini aç veya kapat" -> "Linux için Windows Alt Sistemi" tik

Write-Host "=== GrsProbe ARM Windows CE Derleyici ===" -ForegroundColor Cyan
Write-Host ""

# WSL var mı kontrol et
$wsl = Get-Command wsl -ErrorAction SilentlyContinue
if (-not $wsl) {
    Write-Host "HATA: WSL bulunamadi!" -ForegroundColor Red
    Write-Host ""
    Write-Host "WSL kurmak icin:" -ForegroundColor Yellow
    Write-Host "  1. PowerShell'i Yönetici olarak ac"
    Write-Host "  2. Su komutu calistir: wsl --install"
    Write-Host "  3. Bilgisayari yeniden baslat"
    Write-Host "  4. Bu scripti tekrar calistir"
    Read-Host "Devam etmek icin Enter'a basin"
    exit 1
}

Write-Host "WSL bulundu. Devam ediliyor..." -ForegroundColor Green

# Kaynak dosyanın yolu (Windows path -> WSL path)
$winPath = $PSScriptRoot
$wslPath = $winPath -replace '\\', '/' -replace '^([A-Za-z]):', '/mnt/$1'
$wslPath = $wslPath -replace '^/mnt/([A-Za-z])', { "/mnt/$($args[0].Value.ToLower())" }

# Daha güvenilir path dönüşümü
$drive = $winPath.Substring(0,1).ToLower()
$rest  = $winPath.Substring(2) -replace '\\', '/'
$wslPathFinal = "/mnt/$drive$rest"

Write-Host "Kaynak dizin (WSL): $wslPathFinal" -ForegroundColor Gray
Write-Host ""

# WSL'de toolchain kur ve derle
$buildScript = @"
#!/bin/bash
set -e

echo "[1/4] Toolchain kontrol ediliyor..."

# arm-mingw32ce yoksa kur
if ! command -v arm-mingw32ce-g++ &>/dev/null; then
    echo "[2/4] mingw32ce toolchain kuruluyor (tek seferlik)..."
    # Ubuntu/Debian
    if command -v apt-get &>/dev/null; then
        sudo apt-get update -qq
        # mingw32ce doğrudan apt'de olmayabilir, cegcc kullanacağız
        sudo apt-get install -y -qq gcc-arm-linux-gnueabi binutils-arm-linux-gnueabi 2>/dev/null || true
        
        # Alternatif: cegcc binary indir
        if ! command -v arm-mingw32ce-g++ &>/dev/null; then
            echo "mingw32ce indiriliyor..."
            cd /tmp
            # Sourceforge'dan binary release
            wget -q "https://sourceforge.net/projects/cegcc/files/cegcc/cegcc-current-linux-i386.tar.gz/download" \
                 -O cegcc.tar.gz 2>/dev/null || \
            curl -sL "https://sourceforge.net/projects/cegcc/files/cegcc/cegcc-current-linux-i386.tar.gz/download" \
                 -o cegcc.tar.gz 2>/dev/null || true
            
            if [ -f cegcc.tar.gz ] && [ -s cegcc.tar.gz ]; then
                sudo tar xzf cegcc.tar.gz -C /opt/ 2>/dev/null || true
                export PATH="/opt/cegcc/bin:$PATH"
            fi
        fi
    fi
fi

# Toolchain var mı tekrar kontrol
if command -v arm-mingw32ce-g++ &>/dev/null; then
    echo "[3/4] Derleniyor: arm-mingw32ce-g++"
    cd "$1"
    arm-mingw32ce-g++ \
        -DUNICODE -D_UNICODE \
        -D_WIN32_WCE=0x0500 \
        -DUNDER_CE=0x0500 \
        -DWIN32 -DARM \
        -O1 -Wall \
        -fno-exceptions -fno-rtti \
        -mwindows \
        -o GrsProbe.exe \
        GrsProbe.cpp \
        -lcoredll -lcommctrl
    
    echo "[4/4] Tamamlandi!"
    ls -lh GrsProbe.exe
    echo "SUCCESS"
else
    # Toolchain yoksa alternatif yol: sadece syntax kontrol
    echo "[!] arm-mingw32ce-g++ bulunamadi, alternatif deneniyor..."
    echo ""
    echo "Manuel kurulum gerekiyor. Asagidaki komutu WSL'de calistirin:"
    echo ""
    echo "  sudo apt-get install -y mingw-w64"
    echo ""
    echo "Sonra su komutla derleyin:"
    echo "  i686-w64-mingw32-g++ -D_WIN32_WCE=0x0500 -O1 -mwindows \\"
    echo "    -o GrsProbe_x86.exe GrsProbe.cpp -lcoredll -lcommctrl"
    echo ""
    echo "NOT: Bu x86 WinCE icin derler (ARM degil)."
    echo "ARM icin Visual Studio 2005/2008 Smart Device gerekli."
    echo "MANUAL_NEEDED"
fi
"@

# Temp script dosyasına yaz
$tempScript = "$env:TEMP\build_grsprobe.sh"
$buildScript | Out-File -FilePath $tempScript -Encoding UTF8 -NoNewline

Write-Host "Derleme başlatılıyor..." -ForegroundColor Yellow
$result = wsl bash /mnt/c/Users/$env:USERNAME/AppData/Local/Temp/build_grsprobe.sh "`"$wslPathFinal`""

Write-Host ""
if ($result -contains "SUCCESS") {
    Write-Host "DERLEME BASARILI!" -ForegroundColor Green
    Write-Host "GrsProbe.exe dosyasi olusturuldu." -ForegroundColor Green
    Write-Host ""
    Write-Host "Simdi FTP ile cihaza yukleyin:" -ForegroundColor Cyan
    Write-Host "  ftp <cihaz_ip>"
    Write-Host "  put GrsProbe\GrsProbe.exe \StorageCard\GrsProbe.exe"
} elseif ($result -contains "MANUAL_NEEDED") {
    Write-Host "Toolchain bulunamadi." -ForegroundColor Yellow
    Write-Host "Asagiya bakin: Manuel adimlar" -ForegroundColor Yellow
} else {
    Write-Host "Sonuc:" -ForegroundColor Gray
    $result | ForEach-Object { Write-Host "  $_" }
}

Write-Host ""
Read-Host "Bitirmek icin Enter'a basin"
