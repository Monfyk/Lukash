package android.app;
public class Activity extends android.content.Context {
    protected void onCreate(android.os.Bundle b) {}
    protected void onDestroy() {}
    public void onBackPressed() {}
    public final boolean requestWindowFeature(int f) { return false; }
    public void setContentView(android.view.View v) {}
}
