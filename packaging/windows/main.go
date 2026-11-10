// Акордник для Windows: окреме вікно з вбудованою сторінкою (WebView2).
// Сторінка вшита в exe і роздається з локального сервера 127.0.0.1, тож інтернет не потрібен.
package main

import (
	_ "embed"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"

	webview2 "github.com/jchv/go-webview2"
)

//go:embed www/index.html
var page []byte

func main() {
	// Фіксований порт, щоб збережені таби (localStorage) не губилися між запусками.
	ln, err := net.Listen("tcp", "127.0.0.1:47391")
	if err != nil {
		ln, err = net.Listen("tcp", "127.0.0.1:0")
		if err != nil {
			openInBrowser()
			return
		}
	}
	go http.Serve(ln, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("Cache-Control", "no-store")
		w.Write(page)
	}))
	url := "http://" + ln.Addr().String() + "/"

	dataDir := filepath.Join(os.Getenv("LOCALAPPDATA"), "Akordnyk")
	w := webview2.NewWithOptions(webview2.WebViewOptions{
		AutoFocus: true,
		DataPath:  dataDir,
		WindowOptions: webview2.WindowOptions{
			Title:  "Акордник",
			Width:  1320,
			Height: 900,
			IconId: 1,
			Center: true,
		},
	})
	if w == nil {
		// Немає WebView2 Runtime — відкриваємо у звичайному браузері.
		openInBrowser()
		return
	}
	defer w.Destroy()
	w.SetSize(1000, 640, webview2.HintMin)
	w.Navigate(url)
	w.Run()
}

func openInBrowser() {
	p := filepath.Join(os.TempDir(), "akordnyk.html")
	if err := os.WriteFile(p, page, 0o644); err != nil {
		return
	}
	exec.Command("rundll32", "url.dll,FileProtocolHandler", p).Start()
}
