package dev.t2can.unlock;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.util.Base64;
import android.webkit.WebMessage;
import android.webkit.WebMessagePort;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.util.Locale;
import org.json.JSONObject;

public final class FileExporter {
    private static final long MAX_BYTES = 16L * 1024L * 1024L;

    public interface Launcher {
        void launchCreateDocument(Intent intent);
    }

    private final Activity activity;
    private final Launcher launcher;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private ByteArrayOutputStream pendingBuffer;
    private String pendingName = "p4can-export.txt";
    private boolean awaitDocumentPicker;
    private boolean userCancelled;
    private boolean streamEnded;
    private OutputStream outputStream;
    private Uri activeUri;
    private WebMessagePort replyPort;

    public FileExporter(Activity activity, Launcher launcher) {
        this.activity = activity;
        this.launcher = launcher;
    }

    public void attachPort(WebMessagePort port) {
        replyPort = port;
    }

    public boolean isBusy() {
        return pendingBuffer != null || outputStream != null || awaitDocumentPicker;
    }

    public synchronized void handleBridgeMessage(String rawJson, WebMessagePort port) {
        if (port != null) {
            replyPort = port;
        }
        try {
            JSONObject obj = new JSONObject(rawJson);
            String type = obj.optString("type", "");
            if ("begin".equals(type)) {
                beginTransfer(
                        obj.optString("name", "export.txt"),
                        obj.optLong("size", 0L));
            } else if ("chunk".equals(type)) {
                handleChunk(obj.optString("data", ""));
            } else if ("end".equals(type)) {
                handleEnd();
            } else if ("abort".equals(type)) {
                abortTransfer();
            }
        } catch (Exception e) {
            sendReply(false, e.getMessage());
            cleanupStreams();
        }
    }

    public synchronized void onDocumentPicked(Uri uri) {
        awaitDocumentPicker = false;
        if (uri == null) {
            userCancelled = true;
            cleanupStreams();
            return;
        }
        try {
            OutputStream os = activity.getContentResolver().openOutputStream(uri, "w");
            if (os == null) {
                throw new IOException("无法打开目标文件写入流");
            }
            activeUri = uri;
            outputStream = os;
            if (pendingBuffer != null) {
                if (pendingBuffer.size() > 0) {
                    outputStream.write(pendingBuffer.toByteArray());
                }
                pendingBuffer.close();
                pendingBuffer = null;
            }
            if (streamEnded) {
                finishOutputStream();
            }
        } catch (Exception e) {
            cleanupFailedFile();
            sendReply(false, e.getMessage());
        }
    }

    public void close() {
        cleanupStreams();
    }

    private void beginTransfer(String name, long size) {
        if (size > MAX_BYTES) {
            sendReply(false, "导出文件超过 16MB 上限");
            return;
        }
        cleanupStreams();
        pendingName = sanitizeFileName(name);
        pendingBuffer = new ByteArrayOutputStream();
        awaitDocumentPicker = true;
        userCancelled = false;
        streamEnded = false;

        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType(guessMimeType(pendingName));
        intent.putExtra(Intent.EXTRA_TITLE, pendingName);
        mainHandler.post(new Runnable() {
            @Override
            public void run() {
                try {
                    launcher.launchCreateDocument(intent);
                } catch (Exception e) {
                    awaitDocumentPicker = false;
                    cleanupStreams();
                    sendReply(false, activity.getString(R.string.file_picker_unavailable));
                }
            }
        });
        sendReply(true, null);
    }

    private void handleChunk(String base64) throws IOException {
        if (userCancelled) {
            sendReply(false, "Save cancelled");
            return;
        }
        byte[] bytes = Base64.decode(base64, Base64.DEFAULT);
        if (outputStream != null) {
            outputStream.write(bytes);
        } else if (pendingBuffer != null) {
            if (pendingBuffer.size() + bytes.length > MAX_BYTES) {
                throw new IOException("导出数据超过 16MB 上限");
            }
            pendingBuffer.write(bytes);
        } else {
            throw new IOException("没有活动的传输任务");
        }
        sendReply(true, null);
    }

    private void handleEnd() throws IOException {
        if (userCancelled) {
            sendReply(false, "Save cancelled");
            return;
        }
        streamEnded = true;
        if (outputStream != null) {
            finishOutputStream();
        } else if (!awaitDocumentPicker) {
            throw new IOException("尚未选择保存文件");
        }
    }

    private void abortTransfer() {
        cleanupFailedFile();
        cleanupStreams();
    }

    private void finishOutputStream() throws IOException {
        try {
            if (outputStream != null) {
                outputStream.flush();
                outputStream.close();
            }
        } finally {
            outputStream = null;
            activeUri = null;
            streamEnded = false;
            pendingBuffer = null;
        }
        sendReply(true, null);
    }

    private void cleanupFailedFile() {
        if (activeUri != null) {
            try {
                DocumentsContract.deleteDocument(activity.getContentResolver(), activeUri);
            } catch (Exception ignored) {
            }
        }
        cleanupStreams();
    }

    private void cleanupStreams() {
        try {
            if (outputStream != null) {
                outputStream.close();
            }
        } catch (IOException ignored) {
        }
        try {
            if (pendingBuffer != null) {
                pendingBuffer.close();
            }
        } catch (IOException ignored) {
        }
        outputStream = null;
        pendingBuffer = null;
        activeUri = null;
        awaitDocumentPicker = false;
        streamEnded = false;
    }

    private void sendReply(boolean ok, String error) {
        WebMessagePort port = replyPort;
        if (port == null) {
            return;
        }
        try {
            JSONObject obj = new JSONObject();
            obj.put("ok", ok);
            if (error != null) {
                obj.put("error", error);
            }
            port.postMessage(new WebMessage(obj.toString()));
        } catch (Exception ignored) {
        }
    }

    private static String sanitizeFileName(String name) {
        if (name == null) {
            return "p4can-export.txt";
        }
        String trimmed = name.trim().replaceAll("[\\\\/:*?\"<>|]", "_");
        return trimmed.isEmpty() ? "p4can-export.txt" : trimmed;
    }

    private static String guessMimeType(String name) {
        String lower = name.toLowerCase(Locale.US);
        if (lower.endsWith(".csv")) {
            return "text/csv";
        }
        if (lower.endsWith(".json")) {
            return "application/json";
        }
        if (lower.endsWith(".bin")) {
            return "application/octet-stream";
        }
        return "text/plain";
    }
}
