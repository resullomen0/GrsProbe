/*
 * GrsProbe.cpp
 * Windows CE ARM - Galileo HMI Shared Memory / Process Probe
 *
 * Ne yapar:
 *   - Sistemdeki tüm process'leri listeler
 *   - Her process'in açık handle'larını (FileMapping, Mutex, Event, Semaphore) tarar
 *   - Erişilebilen shared memory bölgelerini okur ve içeriğini hex+ascii olarak kaydeder
 *   - Sonucu \StorageCard\data\grs_probe.xml dosyasına yazar (mevcut olanın üzerine)
 *   - Hafıza kullanımı minimumdur: dosyaya satır satır yazar, büyük buffer tutmaz
 *
 * GUI:
 *   - Tek pencere: "Tara ve Kaydet" butonu + durum metni
 *   - Tarama bitince "Tamam - dosya yazıldı" gösterir
 *   - Kapat butonu ile temiz çıkış
 *
 * Güvenlik:
 *   - Sadece READ erişimi kullanır, hiçbir şeye yazmaz
 *   - Tarama sırasında Galileo process'ine dokunmaz
 *
 * Derleme: ARM Windows CE 5.0/6.0
 *   arm-mingw32ce-g++ GrsProbe.cpp -o GrsProbe.exe -lcoredll -lcommctrl
 *   veya Visual Studio 2005/2008 Smart Device projesi
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>   // Process/thread snapshot
#include <tchar.h>
#include <stdio.h>
#include <string.h>

// ---- Sabitler ----
#define IDC_BTN_SCAN   101
#define IDC_BTN_CLOSE  102
#define IDC_STATIC_STATUS 103

// XML çıktı dosyası - StorageCard'da data klasörü
#define OUTPUT_FILE TEXT("\\StorageCard\\data\\grs_probe.xml")

// Tek bir shared memory bölgesinden okunacak maksimum byte
// Büyük tutmayın - sadece ilk N byte'a bakıyoruz
#define MAX_READ_BYTES  4096

// XML'e yazılacak maksimum hex satır sayısı (her satır 16 byte)
#define MAX_HEX_LINES   64   // = 1024 byte görünür

// ---- Global değişkenler ----
static HWND  g_hWnd        = NULL;
static HWND  g_hBtnScan    = NULL;
static HWND  g_hBtnClose   = NULL;
static HWND  g_hStatus     = NULL;
static HANDLE g_hScanThread = NULL;
static volatile BOOL g_scanning = FALSE;

// ---- Yardımcı: TCHAR -> ASCII (XML için) ----
static void tchar_to_ascii(const TCHAR *src, char *dst, int dst_size)
{
#ifdef UNICODE
    WideCharToMultiByte(CP_ACP, 0, src, -1, dst, dst_size, NULL, NULL);
#else
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
#endif
}

// ---- Yardımcı: XML özel karakterleri escape et ----
static void xml_escape(const char *src, char *dst, int dst_size)
{
    int j = 0;
    for (int i = 0; src[i] && j < dst_size - 6; i++) {
        switch (src[i]) {
            case '&':  memcpy(dst+j, "&amp;",  5); j+=5; break;
            case '<':  memcpy(dst+j, "&lt;",   4); j+=4; break;
            case '>':  memcpy(dst+j, "&gt;",   4); j+=4; break;
            case '"':  memcpy(dst+j, "&quot;", 6); j+=6; break;
            default:
                // Sadece printable ASCII yaz
                if ((unsigned char)src[i] >= 0x20 && (unsigned char)src[i] < 0x7F)
                    dst[j++] = src[i];
                else
                    dst[j++] = '.';
                break;
        }
    }
    dst[j] = '\0';
}

// ---- Dosyaya string yaz (HANDLE üzerinden, malloc yok) ----
static void fwrite_str(HANDLE hf, const char *s)
{
    DWORD written;
    WriteFile(hf, s, (DWORD)strlen(s), &written, NULL);
}

// ---- Shared memory içeriğini XML'e yaz ----
static void dump_sharedmem(HANDLE hFile, const char *name, HANDLE hMap)
{
    // Salt okunur map
    LPVOID pView = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, MAX_READ_BYTES);
    if (!pView) {
        char line[128];
        wsprintfA(line, "    <error>MapViewOfFile failed: %lu</error>\r\n",
                  GetLastError());
        fwrite_str(hFile, line);
        return;
    }

    DWORD size = MAX_READ_BYTES;
    const BYTE *data = (const BYTE*)pView;

    // İlk 8 byte'a bakarak boyutu tahmin et (bazı GRS map'leri başa boyut yazar)
    DWORD declared_size = 0;
    if (size >= 4) {
        declared_size = *(const DWORD*)data;
        char line[64];
        wsprintfA(line, "    <declared_size>%lu</declared_size>\r\n", declared_size);
        fwrite_str(hFile, line);
    }

    // Hex dump - maksimum MAX_HEX_LINES satır
    fwrite_str(hFile, "    <hexdump>\r\n");

    int lines = 0;
    for (DWORD offset = 0; offset < size && lines < MAX_HEX_LINES; offset += 16, lines++) {
        char line[128];
        char hex[64]  = {0};
        char asc[24]  = {0};
        int  hex_pos  = 0;
        int  asc_pos  = 0;

        for (int col = 0; col < 16 && (offset + col) < size; col++) {
            BYTE b = data[offset + col];
            // hex
            hex[hex_pos++] = "0123456789ABCDEF"[b >> 4];
            hex[hex_pos++] = "0123456789ABCDEF"[b & 0xF];
            hex[hex_pos++] = ' ';
            // ascii
            asc[asc_pos++] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        }
        hex[hex_pos] = '\0';
        asc[asc_pos] = '\0';

        wsprintfA(line, "      <r o=\"%04lX\" h=\"%s\" a=\"%s\"/>\r\n",
                  offset, hex, asc);
        fwrite_str(hFile, line);
    }

    fwrite_str(hFile, "    </hexdump>\r\n");

    // Metin gibi görünen kısmı ayrıca çıkar (ilk 256 byte)
    fwrite_str(hFile, "    <text_preview>");
    char preview[256];
    int pi = 0;
    for (DWORD i = 0; i < size && i < 256 && pi < 250; i++) {
        BYTE b = data[i];
        if (b == 0) {
            if (pi > 0 && preview[pi-1] != '|') preview[pi++] = '|';
        } else if (b >= 0x20 && b < 0x7F) {
            preview[pi++] = (char)b;
        }
    }
    preview[pi] = '\0';
    char escaped[512];
    xml_escape(preview, escaped, sizeof(escaped));
    fwrite_str(hFile, escaped);
    fwrite_str(hFile, "</text_preview>\r\n");

    UnmapViewOfFile(pView);
}

// ---- Process listesini XML'e yaz ----
static void scan_processes(HANDLE hFile)
{
    fwrite_str(hFile, "  <processes>\r\n");

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        fwrite_str(hFile, "    <error>Snapshot failed</error>\r\n");
        fwrite_str(hFile, "  </processes>\r\n");
        return;
    }

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);

    if (Process32First(hSnap, &pe)) {
        do {
            char name[MAX_PATH];
            tchar_to_ascii(pe.szExeFile, name, sizeof(name));

            char line[256];
            wsprintfA(line,
                "    <process pid=\"%lu\" threads=\"%lu\" name=\"%s\"/>\r\n",
                pe.th32ProcessID,
                pe.cntThreads,
                name);
            fwrite_str(hFile, line);
        } while (Process32Next(hSnap, &pe));
    }

    CloseHandle(hSnap);
    fwrite_str(hFile, "  </processes>\r\n");
}

// ---- Bilinen GRS shared memory isimlerini dene ----
// Galileo GRS runtime'ın kullandığı olası isimler
static const char * const GRS_MAP_NAMES[] = {
    "GRS_VARDATA",
    "GRS_VARIABLES",
    "GRSW_SHARED",
    "GRSW_DATA",
    "GRS_DATA",
    "GRS_MEM",
    "GRS_GLOBAL",
    "GrsVarMem",
    "GrsSharedMem",
    "GRS_VAR_TABLE",
    "GRS_TAG_DATA",
    "GALILEO_VARS",
    "GALILEO_DATA",
    "GALILEO_MEM",
    "HMI_DATA",
    "HMI_SHARED",
    "HMI_VARS",
    "REGLO_DATA",
    "REGLO_MEM",
    "REGLOCHILL",
    "CHILLER_DATA",
    "BRW_DATA",
    "DANF_DATA",
    "hmi_danf398",
    "hmi_danf398_vars",
    // Sayısal olası isimler
    "Local\\GRS_DATA",
    "Global\\GRS_DATA",
    "Local\\GRSW_SHARED",
    "Global\\GRSW_SHARED",
    NULL  // sentinel
};

static void scan_known_maps(HANDLE hFile)
{
    fwrite_str(hFile, "  <known_maps>\r\n");

    for (int i = 0; GRS_MAP_NAMES[i] != NULL; i++) {
        // OpenFileMapping: sadece READ, mevcut değilse hata döner
        HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, GRS_MAP_NAMES[i]);
        if (hMap != NULL) {
            char line[256];
            wsprintfA(line, "  <map name=\"%s\" found=\"1\">\r\n", GRS_MAP_NAMES[i]);
            fwrite_str(hFile, line);
            dump_sharedmem(hFile, GRS_MAP_NAMES[i], hMap);
            fwrite_str(hFile, "  </map>\r\n");
            CloseHandle(hMap);
        }
        // Bulunamayanları da kaydet (opsiyonel, dosya şişirme)
        // Sadece bulunanları yazıyoruz
    }

    fwrite_str(hFile, "  </known_maps>\r\n");
}

// ---- Sistem bellek bilgisi ----
static void scan_memory_info(HANDLE hFile)
{
    MEMORYSTATUS ms;
    GlobalMemoryStatus(&ms);

    char line[256];
    fwrite_str(hFile, "  <memory_status>\r\n");
    wsprintfA(line, "    <load_percent>%lu</load_percent>\r\n",    ms.dwMemoryLoad);
    fwrite_str(hFile, line);
    wsprintfA(line, "    <total_physical>%lu</total_physical>\r\n", ms.dwTotalPhys);
    fwrite_str(hFile, line);
    wsprintfA(line, "    <avail_physical>%lu</avail_physical>\r\n", ms.dwAvailPhys);
    fwrite_str(hFile, line);
    wsprintfA(line, "    <total_virtual>%lu</total_virtual>\r\n",   ms.dwTotalVirtual);
    fwrite_str(hFile, line);
    wsprintfA(line, "    <avail_virtual>%lu</avail_virtual>\r\n",   ms.dwAvailVirtual);
    fwrite_str(hFile, line);
    fwrite_str(hFile, "  </memory_status>\r\n");
}

// ---- Modül listesi (grsw3.exe'nin yüklü DLL'leri) ----
static void scan_modules(HANDLE hFile)
{
    fwrite_str(hFile, "  <modules>\r\n");

    // Galileo process ID'sini bul
    DWORD grs_pid = 0;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe;
        pe.dwSize = sizeof(PROCESSENTRY32);
        if (Process32First(hSnap, &pe)) {
            do {
                char name[64];
                tchar_to_ascii(pe.szExeFile, name, sizeof(name));
                // küçük harfe çevir karşılaştırma için
                for (int i = 0; name[i]; i++)
                    if (name[i] >= 'A' && name[i] <= 'Z') name[i] += 32;
                if (strstr(name, "grsw3") || strstr(name, "galileo")) {
                    grs_pid = pe.th32ProcessID;
                    char line[128];
                    wsprintfA(line, "    <grs_pid>%lu</grs_pid>\r\n", grs_pid);
                    fwrite_str(hFile, line);
                    break;
                }
            } while (Process32Next(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }

    // Modülleri listele
    HANDLE hModSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, grs_pid);
    if (hModSnap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 me;
        me.dwSize = sizeof(MODULEENTRY32);
        if (Module32First(hModSnap, &me)) {
            do {
                char name[MAX_PATH];
                char path[MAX_PATH];
                tchar_to_ascii(me.szModule,   name, sizeof(name));
                tchar_to_ascii(me.szExePath,  path, sizeof(path));
                char line[512];
                wsprintfA(line,
                    "    <module name=\"%s\" path=\"%s\" base=\"0x%08lX\" size=\"%lu\"/>\r\n",
                    name, path,
                    (DWORD)(DWORD_PTR)me.modBaseAddr,
                    me.modBaseSize);
                fwrite_str(hFile, line);
            } while (Module32Next(hModSnap, &me));
        }
        CloseHandle(hModSnap);
    }

    fwrite_str(hFile, "  </modules>\r\n");
}

// ---- Ana tarama fonksiyonu (ayrı thread'de çalışır) ----
static DWORD WINAPI ScanThread(LPVOID)
{
    // Durum güncelle
    SetWindowText(g_hStatus, TEXT("Taranıyor... lütfen bekleyin"));
    EnableWindow(g_hBtnScan, FALSE);

    // Dosyayı aç (varsa üzerine yaz - hafıza tasarrufu için append yok)
    HANDLE hFile = CreateFile(
        OUTPUT_FILE,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,      // her seferinde sıfırdan yaz
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (hFile == INVALID_HANDLE_VALUE) {
        SetWindowText(g_hStatus, TEXT("HATA: Dosya açılamadı!"));
        EnableWindow(g_hBtnScan, TRUE);
        g_scanning = FALSE;
        return 1;
    }

    // XML başlığı
    fwrite_str(hFile, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n");
    fwrite_str(hFile, "<grs_probe>\r\n");

    // Timestamp (Windows CE'de GetLocalTime)
    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[64];
    wsprintfA(ts, "  <timestamp>%04d-%02d-%02d %02d:%02d:%02d</timestamp>\r\n",
              st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond);
    fwrite_str(hFile, ts);

    // 1. Bellek durumu
    SetWindowText(g_hStatus, TEXT("Bellek bilgisi alınıyor..."));
    scan_memory_info(hFile);

    // 2. Process listesi
    SetWindowText(g_hStatus, TEXT("Process'ler taranıyor..."));
    scan_processes(hFile);

    // 3. Modüller
    SetWindowText(g_hStatus, TEXT("Modüller taranıyor..."));
    scan_modules(hFile);

    // 4. Bilinen shared memory isimleri
    SetWindowText(g_hStatus, TEXT("Shared memory taranıyor..."));
    scan_known_maps(hFile);

    // XML sonu
    fwrite_str(hFile, "</grs_probe>\r\n");

    // Dosyayı kapat - flush otomatik
    CloseHandle(hFile);

    SetWindowText(g_hStatus,
        TEXT("Tamamlandı! \\StorageCard\\data\\grs_probe.xml yazıldı."));
    EnableWindow(g_hBtnScan, TRUE);
    g_scanning = FALSE;
    return 0;
}

// ---- Window Procedure ----
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        // "Tara ve Kaydet" butonu
        g_hBtnScan = CreateWindow(
            TEXT("BUTTON"), TEXT("Tara ve Kaydet"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 10, 200, 40,
            hWnd, (HMENU)IDC_BTN_SCAN,
            ((LPCREATESTRUCT)lParam)->hInstance, NULL);

        // "Kapat" butonu
        g_hBtnClose = CreateWindow(
            TEXT("BUTTON"), TEXT("Kapat"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            220, 10, 100, 40,
            hWnd, (HMENU)IDC_BTN_CLOSE,
            ((LPCREATESTRUCT)lParam)->hInstance, NULL);

        // Durum etiketi
        g_hStatus = CreateWindow(
            TEXT("STATIC"), TEXT("Hazır. 'Tara ve Kaydet' butonuna basın."),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, 60, 400, 60,
            hWnd, (HMENU)IDC_STATIC_STATUS,
            ((LPCREATESTRUCT)lParam)->hInstance, NULL);

        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_SCAN:
            if (!g_scanning) {
                g_scanning = TRUE;
                // Önceki thread handle'ı kapat
                if (g_hScanThread) {
                    CloseHandle(g_hScanThread);
                    g_hScanThread = NULL;
                }
                DWORD tid;
                g_hScanThread = CreateThread(NULL, 0, ScanThread, NULL, 0, &tid);
                if (!g_hScanThread) {
                    SetWindowText(g_hStatus, TEXT("HATA: Thread oluşturulamadı!"));
                    g_scanning = FALSE;
                }
            }
            break;

        case IDC_BTN_CLOSE:
            // Tarama devam ediyorsa uyar
            if (g_scanning) {
                MessageBox(hWnd,
                    TEXT("Tarama devam ediyor, lütfen bekleyin."),
                    TEXT("GrsProbe"), MB_OK | MB_ICONWARNING);
            } else {
                PostQuitMessage(0);
            }
            break;
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ---- WinMain ----
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPTSTR, int nCmdShow)
{
    // Pencere sınıfı
    WNDCLASS wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = TEXT("GrsProbeWnd");
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClass(&wc);

    // Pencereyi oluştur - küçük tutuyoruz, HMI ekranı küçük
    g_hWnd = CreateWindow(
        TEXT("GrsProbeWnd"),
        TEXT("GRS Probe v1.0"),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        10, 10, 340, 150,
        NULL, NULL, hInst, NULL);

    if (!g_hWnd) return 1;

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    // Mesaj döngüsü
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Temizlik
    if (g_hScanThread) CloseHandle(g_hScanThread);

    return (int)msg.wParam;
}
