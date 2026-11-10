package android.webkit;
public class WebView extends android.view.View {
    public WebView(android.content.Context c) { super(c); }
    public WebSettings getSettings() { return null; }
    public void loadUrl(String u) {}
    public boolean canGoBack() { return false; }
    public void goBack() {}
    public void destroy() {}
}
