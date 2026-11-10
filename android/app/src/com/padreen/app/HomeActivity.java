package com.padreen.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.CompoundButton;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Home screen: finds the PCs on the local network (UDP discovery), shows the ones this tablet is paired with, pairs with a new one by the
 * 9-digit code shown in its pairing window (custom keypad - no system keyboard on an e-ink panel), and connects.  Black on white, big
 * targets, nothing animated, the list is only rebuilt when it really changed (every redraw is an e-ink refresh).
 */
public class HomeActivity extends Activity {
    static final String TAG = "Padreen";
    private static final int FG = Color.BLACK, BG = Color.WHITE, GRAY = 0xFF666666;
    /**
     * The app of the former product (renamed Padreen) may still be installed on the tablet next to this one: while it is, the home screen
     * shows a row that offers to uninstall it.  These two strings are the only place that still names the old product.
     */
    private static final String LEGACY_PACKAGE = "com.paperdisplay.app";
    private static final String LEGACY_LABEL = "PaperDisplay";

    private final Handler ui = new Handler(Looper.getMainLooper());
    private PcStore store;
    private Discovery discovery;
    private final Map<String, Discovery.PcInfo> found = new LinkedHashMap<String, Discovery.PcInfo>();
    private LinearLayout list;
    private TextView status;
    private FrameLayout root;
    private View overlay;
    private String lastSignature = "";
    private volatile boolean connecting;
    private DisplayHub hub;
    private LinearLayout banner;                    // shown while a session is running (or being re-established)
    private TextView bannerText;
    private Button bannerBack;
    private LinearLayout legacyRow;                 // shown while the former product's app is still installed
    private long nextAutoAt;                       // uptimeMillis before which no automatic connection attempt is made
    private boolean resumed;

    // ------------------------------------------------------------------------------------------------------------ lifecycle
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        store = new PcStore(this);
        hub = DisplayHub.get(this);
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        root = new FrameLayout(this);
        root.setBackgroundColor(BG);
        setContentView(root);
        buildMain();
        discovery = new Discovery(Secure.deviceId(this), new Discovery.Listener() {
            @Override
            public void onPc(final Discovery.PcInfo pc) {
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        found.put(Secure.toHex(pc.id), pc);
                    }
                });
            }
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        hideSystemUi();
        connecting = false;
        nextAutoAt = 0;
        hub.addListener(hubListener);
        updateBanner();
        updateLegacyRow();                                  // the old app may have been uninstalled meanwhile (the system's uninstall dialog just closed)
        discovery.start();
        lastSignature = "";
        ui.removeCallbacks(tick);
        ui.post(tick);
        setStatus("搜索中…");
    }

    @Override
    protected void onPause() {
        resumed = false;
        hub.removeListener(hubListener);
        discovery.stop();
        ui.removeCallbacks(tick);
        super.onPause();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemUi();
    }

    private void hideSystemUi() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    @Override
    public void onBackPressed() {
        if (overlay != null) {
            closeOverlay();
            return;
        }
        super.onBackPressed();
    }

    /**
     * Same rule as in the display (see {@link MainActivity#dispatchKeyEvent}): while a PC is attached - the connection banner is up - the
     * hardware volume keys belong to the PC and never change the tablet's volume.  With no PC they work on the tablet as usual.
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent ev) {
        if (hub.receiver.handleVolumeKey(ev)) return true;
        return super.dispatchKeyEvent(ev);
    }

    /** Shows a dialog.  A dialog has its own window and its key events do not pass through the activity, so the volume keys are taken here too. */
    private void showDialog(AlertDialog.Builder b) {
        AlertDialog d = b.create();
        d.setOnKeyListener(new DialogInterface.OnKeyListener() {
            @Override
            public boolean onKey(DialogInterface dialog, int keyCode, KeyEvent ev) {
                return hub.receiver.handleVolumeKey(ev);
            }
        });
        d.show();
    }

    // ------------------------------------------------------------------------------------------------------------ session banner
    private final DisplayHub.Listener hubListener = new DisplayHub.Listener() {
        @Override
        public void onSessionChanged() {
            ui.post(new Runnable() {
                @Override
                public void run() {
                    updateBanner();
                }
            });
        }
    };

    private void updateBanner() {
        if (banner == null) return;
        if (hub.active()) {
            bannerText.setText("● 已连接：" + hub.label() + ("LAN".equals(hub.kind()) ? "（无线）" : "（USB）"));
            bannerBack.setVisibility(View.VISIBLE);
            banner.setVisibility(View.VISIBLE);
        } else if (hub.reconnecting()) {
            bannerText.setText("○ 重连中…");
            bannerBack.setVisibility(View.GONE);
            banner.setVisibility(View.VISIBLE);
        } else {
            banner.setVisibility(View.GONE);
        }
    }

    // ------------------------------------------------------------------------------------------------------------ the old app
    private boolean legacyInstalled() {
        try {
            getPackageManager().getPackageInfo(LEGACY_PACKAGE, 0);
            return true;
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        } catch (RuntimeException e) {                          // the package manager does not answer (right now): do not nag
            return false;
        }
    }

    /** The "remove the old app" row is up while the old app is installed (the view only changes - an e-ink redraw - when that changes). */
    private void updateLegacyRow() {
        if (legacyRow == null) return;
        legacyRow.setVisibility(legacyInstalled() ? View.VISIBLE : View.GONE);
    }

    /** Opens the system's own uninstall confirmation for the old app (needs REQUEST_DELETE_PACKAGES). */
    private void uninstallLegacy() {
        try {
            startActivity(new Intent(Intent.ACTION_DELETE, Uri.parse("package:" + LEGACY_PACKAGE)));
        } catch (Exception e) {
            android.util.Log.w(TAG, "cannot open the uninstall dialog: " + e);
            setStatus("无法打开卸载界面");
        }
    }

    // ------------------------------------------------------------------------------------------------------------ list refresh
    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            if (!resumed) return;
            long now = android.os.SystemClock.uptimeMillis();
            for (java.util.Iterator<Map.Entry<String, Discovery.PcInfo>> it = found.entrySet().iterator(); it.hasNext(); ) {
                if (now - it.next().getValue().seen > 6000) it.remove();
            }
            rebuildList(false);
            maybeAutoConnect();
            if (legacyRow.getVisibility() == View.VISIBLE) updateLegacyRow();     // after the uninstall dialog: the row goes away as soon as the package is gone
            ui.postDelayed(this, 1000);
        }
    };

    /** Connect by itself to the PC used last, when it shows up: not after a deliberate Back, not right after the PC ended a session, retried every 15 s. */
    private void maybeAutoConnect() {
        if (connecting || overlay != null || !store.autoConnect() || Handoff.userEnded() || Handoff.coolingDown()) return;
        if (hub.active() || hub.reconnecting()) return;                       // already connected (or re-connecting) to a PC
        long now = android.os.SystemClock.uptimeMillis();
        if (now < nextAutoAt) return;
        PcStore.Pc best = null;                                    // the paired PC that is online now (most recently used first)
        Discovery.PcInfo bestInfo = null;
        for (PcStore.Pc p : store.list()) {
            Discovery.PcInfo i = found.get(p.idHex());
            if (i == null || i.busy) continue;
            if (best == null || p.lastUsed > best.lastUsed) {
                best = p;
                bestInfo = i;
            }
        }
        if (best == null) return;
        nextAutoAt = now + 15000;
        connectTo(best, bestInfo.ip, bestInfo.port, true);
    }

    private static final class Row {
        PcStore.Pc saved;
        Discovery.PcInfo online;
        String name;
    }

    private void rebuildList(boolean force) {
        List<Row> rows = new ArrayList<Row>();
        List<PcStore.Pc> saved = store.list();
        for (PcStore.Pc p : saved) {
            Row r = new Row();
            r.saved = p;
            r.online = found.get(p.idHex());
            r.name = r.online != null ? r.online.name : p.name;
            rows.add(r);
        }
        for (Map.Entry<String, Discovery.PcInfo> e : found.entrySet()) {
            boolean isSaved = false;
            for (PcStore.Pc p : saved) isSaved = isSaved || p.idHex().equals(e.getKey());
            if (!isSaved) {
                Row r = new Row();
                r.online = e.getValue();
                r.name = r.online.name;
                rows.add(r);
            }
        }
        for (Row r : rows) {                                       // two computers with the same name: tell them apart by a piece of their id
            int same = 0;
            for (Row o : rows) if (o.name.equals(r.name)) same++;
            if (same > 1) r.name = r.name + " (" + (r.saved != null ? r.saved.idHex() : Secure.toHex(r.online.id)).substring(0, 4) + ")";
        }
        Collections.sort(rows, new Comparator<Row>() {
            @Override
            public int compare(Row a, Row b) {
                int oa = a.online != null ? 0 : 1, ob = b.online != null ? 0 : 1;
                if (oa != ob) return oa - ob;
                long la = a.saved != null ? a.saved.lastUsed : 0, lb = b.saved != null ? b.saved.lastUsed : 0;
                return lb > la ? 1 : lb < la ? -1 : a.name.compareTo(b.name);
            }
        });
        StringBuilder sig = new StringBuilder();
        for (Row r : rows) sig.append(r.name).append('|').append(r.saved != null).append('|').append(r.online != null ? r.online.ip + r.online.busy + r.online.pairing : "off").append(';');
        sig.append(store.autoConnect());
        if (!force && sig.toString().equals(lastSignature)) return;
        lastSignature = sig.toString();
        list.removeAllViews();
        if (rows.isEmpty()) {
            TextView t = text("没有找到电脑", 24, false, GRAY);
            t.setPadding(dp(8), dp(24), dp(8), dp(8));
            list.addView(t);
            return;
        }
        for (Row r : rows) list.addView(buildRow(r));
    }

    private View buildRow(final Row r) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(16), dp(14), dp(16), dp(14));
        row.setBackground(box(BG, FG, dp(2), dp(10)));
        LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(-1, -2);
        rlp.setMargins(0, dp(8), 0, dp(8));
        row.setLayoutParams(rlp);

        LinearLayout left = new LinearLayout(this);
        left.setOrientation(LinearLayout.VERTICAL);
        left.addView(text(r.name, 28, true, FG));
        String sub;
        if (r.online == null) sub = "离线";
        else if (r.online.busy) sub = r.online.ip + "  ·  使用中";
        else sub = r.online.ip + (r.saved != null ? "  ·  已配对" : r.online.pairing ? "  ·  等待配对" : "  ·  未配对");
        left.addView(text(sub, 19, false, GRAY));
        row.addView(left, new LinearLayout.LayoutParams(0, -2, 1f));

        if (r.saved != null) {
            Button forget = button("忘记", 20, dp(110), dp(54));
            forget.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    confirmForget(r.saved);
                }
            });
            LinearLayout.LayoutParams flp = new LinearLayout.LayoutParams(dp(110), dp(54));
            flp.rightMargin = dp(12);
            row.addView(forget, flp);
        }
        if (r.online != null && !r.online.busy) {
            Button go = button(r.saved != null ? "连接" : "配对…", 24, dp(150), dp(62));
            go.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    if (connecting) return;
                    if (r.saved != null) connectTo(r.saved, r.online.ip, r.online.port, false);
                    else showPairing(r.online);
                }
            });
            row.addView(go, new LinearLayout.LayoutParams(dp(150), dp(62)));
        }
        return row;
    }

    // ------------------------------------------------------------------------------------------------------------ main layout
    private void buildMain() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(36), dp(28), dp(36), dp(24));

        LinearLayout head = new LinearLayout(this);
        head.setGravity(Gravity.CENTER_VERTICAL);
        ImageView icon = new ImageView(this);
        try {
            icon.setImageResource(getApplicationInfo().icon);
        } catch (Throwable ignored) {
        }
        head.addView(icon, new LinearLayout.LayoutParams(dp(64), dp(64)));
        LinearLayout titles = new LinearLayout(this);
        titles.setOrientation(LinearLayout.VERTICAL);
        titles.setPadding(dp(16), 0, 0, 0);
        titles.addView(text("Padreen", 34, true, FG));
        String ver = "";
        try {
            ver = "v" + getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception ignored) {
        }
        titles.addView(text(ver, 19, false, GRAY));
        head.addView(titles, new LinearLayout.LayoutParams(0, -2, 1f));
        Button refresh = button("搜索", 22, dp(130), dp(60));
        refresh.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                found.clear();
                Handoff.clearUserEnded();
                nextAutoAt = 0;
                rebuildList(true);
                setStatus("搜索中…");
            }
        });
        head.addView(refresh, new LinearLayout.LayoutParams(dp(130), dp(60)));
        col.addView(head);

        View line = new View(this);
        line.setBackgroundColor(FG);
        LinearLayout.LayoutParams llp = new LinearLayout.LayoutParams(-1, dp(3));
        llp.setMargins(0, dp(18), 0, dp(10));
        col.addView(line, llp);

        // a hint row (gray, bordered like the list rows): the former product's app is still installed next to this one
        legacyRow = new LinearLayout(this);
        legacyRow.setOrientation(LinearLayout.HORIZONTAL);
        legacyRow.setGravity(Gravity.CENTER_VERTICAL);
        legacyRow.setPadding(dp(16), dp(8), dp(12), dp(8));
        legacyRow.setBackground(box(BG, GRAY, dp(2), dp(10)));
        legacyRow.addView(text("旧版「" + LEGACY_LABEL + "」", 20, false, GRAY), new LinearLayout.LayoutParams(0, -2, 1f));
        Button removeOld = button("卸载", 22, dp(130), dp(54));
        removeOld.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                uninstallLegacy();
            }
        });
        LinearLayout.LayoutParams rolp = new LinearLayout.LayoutParams(dp(130), dp(54));
        rolp.leftMargin = dp(12);
        legacyRow.addView(removeOld, rolp);
        LinearLayout.LayoutParams lrlp = new LinearLayout.LayoutParams(-1, -2);
        lrlp.setMargins(0, 0, 0, dp(10));
        col.addView(legacyRow, lrlp);
        updateLegacyRow();                                         // up from the first frame when the old app is installed (onResume checks again)
        banner = new LinearLayout(this);
        banner.setOrientation(LinearLayout.HORIZONTAL);
        banner.setGravity(Gravity.CENTER_VERTICAL);
        banner.setPadding(dp(16), dp(10), dp(16), dp(10));
        banner.setBackground(box(0xFFEFEFEF, FG, dp(3), dp(10)));
        bannerText = text("", 24, true, FG);
        banner.addView(bannerText, new LinearLayout.LayoutParams(0, -2, 1f));
        bannerBack = button("返回显示", 22, dp(170), dp(58));
        bannerBack.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startActivity(new Intent(HomeActivity.this, MainActivity.class).putExtra("lan", true));
            }
        });
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(dp(170), dp(58));
        blp.rightMargin = dp(12);
        banner.addView(bannerBack, blp);
        Button bannerStop = button("断开", 22, dp(130), dp(58));
        bannerStop.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                hub.disconnect();
                setStatus("已断开");
            }
        });
        banner.addView(bannerStop, new LinearLayout.LayoutParams(dp(130), dp(58)));
        LinearLayout.LayoutParams bnlp = new LinearLayout.LayoutParams(-1, -2);
        bnlp.setMargins(0, 0, 0, dp(10));
        col.addView(banner, bnlp);
        banner.setVisibility(View.GONE);

        col.addView(text("电脑", 24, true, FG));
        ScrollView sv = new ScrollView(this);
        sv.setVerticalScrollBarEnabled(false);
        list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        sv.addView(list, new ViewGroup.LayoutParams(-1, -2));
        col.addView(sv, new LinearLayout.LayoutParams(-1, 0, 1f));

        LinearLayout bar = new LinearLayout(this);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        Button manual = button("输入 IP", 22, dp(170), dp(60));
        manual.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showManualAddress();
            }
        });
        bar.addView(manual, new LinearLayout.LayoutParams(dp(170), dp(60)));
        CheckBox auto = new CheckBox(this);
        auto.setText("自动连接");
        auto.setTextSize(22);
        auto.setTextColor(FG);
        auto.setChecked(store.autoConnect());
        auto.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton b, boolean on) {
                store.setAutoConnect(on);
                Handoff.clearUserEnded();
                nextAutoAt = 0;
            }
        });
        LinearLayout.LayoutParams alp = new LinearLayout.LayoutParams(0, -2, 1f);
        alp.leftMargin = dp(28);
        bar.addView(auto, alp);
        col.addView(bar);

        CheckBox gest = new CheckBox(this);
        gest.setText("三指手势");
        gest.setTextSize(22);
        gest.setTextColor(FG);
        gest.setChecked(store.gestures());
        gest.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton b, boolean on) {
                store.setGestures(on);
            }
        });
        LinearLayout.LayoutParams glp = new LinearLayout.LayoutParams(-1, -2);
        glp.topMargin = dp(10);
        col.addView(gest, glp);

        CheckBox rot = new CheckBox(this);
        rot.setText("自动旋转");
        rot.setTextSize(22);
        rot.setTextColor(FG);
        rot.setChecked(store.autoRotate());
        rot.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton b, boolean on) {
                store.setAutoRotate(on);
            }
        });
        LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(-1, -2);
        rlp.topMargin = dp(10);
        col.addView(rot, rlp);

        CheckBox vol = new CheckBox(this);
        vol.setText("音量键控制电脑");
        vol.setTextSize(22);
        vol.setTextColor(FG);
        vol.setChecked(store.volumeToPc());
        vol.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton b, boolean on) {
                store.setVolumeToPc(on);
            }
        });
        LinearLayout.LayoutParams vlp = new LinearLayout.LayoutParams(-1, -2);
        vlp.topMargin = dp(10);
        col.addView(vol, vlp);

        status = text("", 21, false, FG);
        status.setPadding(0, dp(14), 0, 0);
        col.addView(status);
        root.addView(col, new FrameLayout.LayoutParams(-1, -1));
    }

    private void setStatus(String s) {
        if (status != null && !s.contentEquals(status.getText())) status.setText(s);
    }

    // ------------------------------------------------------------------------------------------------------------ connecting
    private void connectTo(final PcStore.Pc pc, final String ip, final int port, final boolean auto) {
        if (connecting) return;
        connecting = true;
        setStatus("连接 " + pc.name + "…");
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    final PcClient.Result r = PcClient.connect(HomeActivity.this, pc, ip, port);
                    store.markUsed(pc.id, ip, port);
                    ui.post(new Runnable() {
                        @Override
                        public void run() {
                            hub.startLan(r);
                            startActivity(new Intent(HomeActivity.this, MainActivity.class).putExtra("lan", true));
                        }
                    });
                } catch (final PcClient.Failure f) {
                    fail(failureText(f), f.reason == PcClient.FAIL_UNPAIRED || f.reason == PcClient.FAIL_AUTH ? pc : null);
                } catch (final IOException e) {
                    fail("连接失败：" + (e.getMessage() == null ? e.toString() : e.getMessage()), null);
                }
            }
        }, "pd-connect").start();
    }

    private void fail(final String msg, final PcStore.Pc repair) {
        ui.post(new Runnable() {
            @Override
            public void run() {
                connecting = false;
                setStatus(msg);
                if (repair != null) offerRepair(repair);
            }
        });
    }

    private static String failureText(PcClient.Failure f) {
        switch (f.reason) {
            case PcClient.FAIL_UNPAIRED:
                return "未配对，请重新配对";
            case PcClient.FAIL_AUTH:
                return "配对失效，请忘记后重新配对";
            case PcClient.FAIL_BUSY:
                return "电脑正在被使用";
            case PcClient.FAIL_NOT_PAIRING:
                return "电脑没有开启配对";
            case PcClient.FAIL_CODE:
                return "配对码错误";
            case PcClient.FAIL_DISABLED:
                return "电脑关闭了 Wi-Fi 连接";
            default:
                return "连接被拒绝（" + f.reason + "）";
        }
    }

    private void offerRepair(final PcStore.Pc pc) {
        showDialog(new AlertDialog.Builder(this).setTitle("配对失效").setMessage("忘记 " + pc.name + " 并重新配对？")
                .setPositiveButton("重新配对", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        store.remove(pc.id);
                        rebuildList(true);
                        Discovery.PcInfo info = found.get(pc.idHex());
                        if (info != null) showPairing(info);
                    }
                }).setNegativeButton("取消", null));
    }

    private void confirmForget(final PcStore.Pc pc) {
        showDialog(new AlertDialog.Builder(this).setTitle("忘记 " + pc.name + "？")
                .setPositiveButton("忘记", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        store.remove(pc.id);
                        rebuildList(true);
                    }
                }).setNegativeButton("取消", null));
    }

    // ------------------------------------------------------------------------------------------------------------ pairing (code) / manual IP
    private StringBuilder entry;
    private TextView entryView, overlayMsg;
    private boolean submitting;                    // a code is being checked: further key presses are ignored

    private void showPairing(final Discovery.PcInfo pc) {
        openKeypad("配对：" + pc.name, "输入电脑上显示的配对码", 9, false, new KeypadDone() {
            @Override
            public void done(final String code) {
                overlayMsg.setText("配对中…");
                new Thread(new Runnable() {
                    @Override
                    public void run() {
                        try {
                            final PcStore.Pc paired = PcClient.pair(HomeActivity.this, pc.ip, pc.port, code);
                            store.put(paired);
                            ui.post(new Runnable() {
                                @Override
                                public void run() {
                                    closeOverlay();
                                    rebuildList(true);
                                    connectTo(paired, pc.ip, pc.port, false);
                                }
                            });
                        } catch (final PcClient.Failure f) {
                            ui.post(new Runnable() {
                                @Override
                                public void run() {
                                    entry.setLength(0);
                                    submitting = false;
                                    renderEntry(9, false);
                                    overlayMsg.setText(failureText(f));
                                }
                            });
                        } catch (final IOException e) {
                            ui.post(new Runnable() {
                                @Override
                                public void run() {
                                    entry.setLength(0);
                                    submitting = false;
                                    renderEntry(9, false);
                                    overlayMsg.setText("配对失败：" + e.getMessage());
                                }
                            });
                        }
                    }
                }, "pd-pair").start();
            }
        });
    }

    private void showManualAddress() {
        openKeypad("输入电脑 IP", "例如 192.168.1.20", 15, true, new KeypadDone() {
            @Override
            public void done(String ip) {
                discovery.addHost(ip);
                closeOverlay();
                setStatus("查询 " + ip + "…");
            }
        });
    }

    private interface KeypadDone {
        void done(String value);
    }

    private void openKeypad(String title, String hint, final int maxLen, final boolean allowDot, final KeypadDone done) {
        entry = new StringBuilder();
        submitting = false;
        FrameLayout o = new FrameLayout(this);
        o.setBackgroundColor(BG);
        o.setClickable(true);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setGravity(Gravity.CENTER_HORIZONTAL);
        col.setPadding(dp(36), dp(24), dp(36), dp(24));
        col.addView(text(title, 32, true, FG));
        TextView h = text(hint, 20, false, GRAY);
        h.setGravity(Gravity.CENTER);
        h.setPadding(0, dp(8), 0, dp(10));
        col.addView(h);
        entryView = text("", 52, true, FG);
        entryView.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        entryView.setGravity(Gravity.CENTER);
        entryView.setBackground(box(BG, FG, dp(3), dp(10)));
        entryView.setPadding(dp(24), dp(10), dp(24), dp(10));
        col.addView(entryView, new LinearLayout.LayoutParams(dp(700), -2));
        overlayMsg = text(" ", 20, false, FG);
        overlayMsg.setPadding(0, dp(10), 0, dp(10));
        overlayMsg.setGravity(Gravity.CENTER);
        col.addView(overlayMsg);

        String[][] keys = allowDot ? new String[][]{{"1", "2", "3"}, {"4", "5", "6"}, {"7", "8", "9"}, {".", "0", "⌫"}}
                : new String[][]{{"1", "2", "3"}, {"4", "5", "6"}, {"7", "8", "9"}, {"取消", "0", "⌫"}};
        for (String[] rowKeys : keys) {
            LinearLayout r = new LinearLayout(this);
            r.setGravity(Gravity.CENTER);
            for (final String k : rowKeys) {
                Button b = button(k, 34, dp(150), dp(84));
                b.setOnClickListener(new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        if (submitting) return;
                        if (k.equals("⌫")) {
                            if (entry.length() > 0) entry.setLength(entry.length() - 1);
                        } else if (k.equals("取消")) {
                            closeOverlay();
                            return;
                        } else if (entry.length() < maxLen) {
                            entry.append(k);
                        }
                        renderEntry(maxLen, allowDot);
                        if (!allowDot && entry.length() == maxLen) {
                            submitting = true;
                            android.util.Log.i(TAG, "code entered (" + entry.length() + " digits), checking");
                            done.done(entry.toString());
                        }
                    }
                });
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(150), dp(84));
                lp.setMargins(dp(8), dp(6), dp(8), dp(6));
                r.addView(b, lp);
            }
            col.addView(r);
        }
        LinearLayout bottom = new LinearLayout(this);
        bottom.setGravity(Gravity.CENTER);
        if (allowDot) {
            Button cancel = button("取消", 26, dp(200), dp(64));
            cancel.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    closeOverlay();
                }
            });
            Button ok = button("确定", 26, dp(200), dp(64));
            ok.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    String s = entry.toString();
                    if (s.matches("\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}")) done.done(s);
                    else overlayMsg.setText("格式不对");
                }
            });
            LinearLayout.LayoutParams a = new LinearLayout.LayoutParams(dp(200), dp(64));
            a.setMargins(dp(12), dp(8), dp(12), 0);
            bottom.addView(cancel, a);
            bottom.addView(ok, a);
        }
        col.addView(bottom);
        o.addView(col, new FrameLayout.LayoutParams(-1, -1));
        overlay = o;
        root.addView(o, new FrameLayout.LayoutParams(-1, -1));
        renderEntry(maxLen, allowDot);
    }

    private void renderEntry(int maxLen, boolean allowDot) {
        String s = entry.toString();
        if (allowDot) {
            entryView.setText(s.isEmpty() ? " " : s);
        } else {
            StringBuilder v = new StringBuilder();
            for (int i = 0; i < maxLen; i++) {
                v.append(i < s.length() ? s.charAt(i) : '_');
                if (i % 3 == 2 && i < maxLen - 1) v.append("  ");
            }
            entryView.setText(v.toString());
        }
    }

    private void closeOverlay() {
        if (overlay != null) {
            root.removeView(overlay);
            overlay = null;
        }
    }

    // ------------------------------------------------------------------------------------------------------------ view helpers
    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density + 0.5f);
    }

    private TextView text(String s, int sp, boolean bold, int color) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return t;
    }

    private GradientDrawable box(int fill, int stroke, int strokePx, int radiusPx) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setStroke(strokePx, stroke);
        g.setCornerRadius(radiusPx);
        return g;
    }

    /** A bordered button that inverts while pressed (no ripple, no animation: e-ink). */
    private Button button(String label, int sp, int w, int h) {
        Button b = new Button(this);
        b.setText(label);
        b.setTextSize(sp);
        b.setAllCaps(false);
        b.setTextColor(new android.content.res.ColorStateList(new int[][]{{android.R.attr.state_pressed}, {}}, new int[]{BG, FG}));
        StateListDrawable sl = new StateListDrawable();
        sl.addState(new int[]{android.R.attr.state_pressed}, box(FG, FG, dp(3), dp(12)));
        sl.addState(new int[]{}, box(BG, FG, dp(3), dp(12)));
        b.setBackground(sl);
        b.setStateListAnimator(null);
        b.setPadding(dp(8), 0, dp(8), 0);
        b.setMinWidth(0);
        b.setMinHeight(0);
        return b;
    }
}
