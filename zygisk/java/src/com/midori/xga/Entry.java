package com.midori.xga;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Forces the GMS nearby flags that let Quick Share use external sharing providers,
 * so GMS binds com.google.android.mosey (Quick Share Extension) for AirDrop.
 * Flag methods are found by id with DexScanner, since GMS obfuscates them.
 * Nearby reads these flags while GMS starts, so the hooks are placed from
 * Instrumentation.newApplication(), before any GMS code runs.
 * Only loaded when GMS Tweaks is absent: two LSPlant engines in GMS make it hang.
 */
public class Entry {
    private static final String TAG = "XGAirDrop";
    // Nearby flags GMS checks before it binds an external sharing provider.
    private static final String[] FORCED_TRUE_FLAGS = {
        "45802344",  // isProviderEnabled accepts external providers
        "45704091",  // provider binding manager
    };

    private static final String CACHE_NAME = "xga_flag_cache";

    public static native boolean nativeHook(Method target, Method callback, Object hookerObj);
    public static native Method nativeHookBackup(Method target, Method callback, Object hookerObj);

    private static volatile Method newApplicationOrig;
    private static final AtomicBoolean FLAGS_HOOKED = new AtomicBoolean();

    // LSPlant calls `Object name(Object[] args)` on the hooker; args[0] is `this`.
    public static class Hooker {
        public Object forceTrue(Object[] args) { return Boolean.TRUE; }

        // Instrumentation.newApplication(ClassLoader, String, Context)
        public Object newApplication(Object[] args) throws Throwable {
            if (args.length > 1 && args[1] instanceof ClassLoader) {
                hookFlags((ClassLoader) args[1]);
            }
            try {
                return newApplicationOrig.invoke(args[0], Arrays.copyOfRange(args, 1, args.length));
            } catch (InvocationTargetException e) {
                throw e.getCause();
            }
        }
    }

    private static final Hooker HOOKER = new Hooker();

    public static void entry(String processName, ClassLoader bootLoader) {
        log("[*] entry() process=" + processName);
        if (hookNewApplication()) return;
        log("[!] newApplication hook unavailable; falling back to polling");
        Thread t = new Thread(() -> {
            try {
                ClassLoader appLoader = waitForAppClassLoader(60, 500);
                if (appLoader == null) {
                    log("[X] Timed out waiting for GMS app ClassLoader");
                    return;
                }
                hookFlags(appLoader);
            } catch (Throwable th) {
                log("[X] Hook thread failed: " + th);
            }
        }, "xga-hook");
        t.setDaemon(true);
        t.start();
    }

    private static ClassLoader waitForAppClassLoader(int tries, long sleepMs) {
        for (int i = 0; i < tries; i++) {
            try {
                Class<?> atClass = Class.forName("android.app.ActivityThread");
                Method m = atClass.getDeclaredMethod("currentApplication");
                m.setAccessible(true);
                Object app = m.invoke(null);
                if (app != null) {
                    ClassLoader cl = (ClassLoader) app.getClass().getMethod("getClassLoader").invoke(app);
                    if (cl != null && findGmsBaseApk(cl) != null) return cl;
                }
            } catch (Throwable ignored) {}
            try { Thread.sleep(sleepMs); } catch (InterruptedException e) { return null; }
        }
        return null;
    }

    private static boolean hookNewApplication() {
        try {
            Method target = Class.forName("android.app.Instrumentation").getDeclaredMethod("newApplication",
                    ClassLoader.class, String.class, Class.forName("android.content.Context"));
            Method callback = Hooker.class.getDeclaredMethod("newApplication", Object[].class);
            callback.setAccessible(true);
            Method orig = nativeHookBackup(target, callback, HOOKER);
            if (orig == null) return false;
            orig.setAccessible(true);
            newApplicationOrig = orig;
            return true;
        } catch (Throwable t) {
            log("[X] newApplication hook failed: " + t);
            return false;
        }
    }

    private static void hookFlags(ClassLoader gmsLoader) {
        if (FLAGS_HOOKED.getAndSet(true)) return;
        String apkPath = findGmsBaseApk(gmsLoader);
        if (apkPath == null) {
            log("[X] GMS base.apk not found");
            return;
        }
        File apk = new File(apkPath);
        String key = apkPath + "|" + apk.length() + "|" + apk.lastModified();
        Map<String, String[]> cache = readCache(key);
        boolean dirty = false;
        for (String flag : FORCED_TRUE_FLAGS) {
            String[] cached = cache.get(flag);
            if (cached != null && hookMethod(gmsLoader, apkPath, flag, cached[0], cached[1])) continue;
            String[] found = scanFlag(apkPath, flag);
            if (found != null && hookMethod(gmsLoader, apkPath, flag, found[0], found[1])) {
                cache.put(flag, found);
                dirty = true;
            }
        }
        if (dirty) writeCache(key, cache);
    }

    private static File cacheFile() {
        int user = 0;
        try {
            user = (Integer) Class.forName("android.os.Process").getMethod("myUid").invoke(null) / 100000;
        } catch (Throwable ignored) {}
        return new File("/data/user_de/" + user + "/com.google.android.gms/no_backup/" + CACHE_NAME);
    }

    private static Map<String, String[]> readCache(String key) {
        Map<String, String[]> map = new HashMap<>();
        try (BufferedReader br = new BufferedReader(new FileReader(cacheFile()))) {
            if (!key.equals(br.readLine())) return map;
            String line;
            while ((line = br.readLine()) != null) {
                String[] f = line.split(" ");
                if (f.length == 3) map.put(f[0], new String[]{f[1], f[2]});
            }
        } catch (Throwable ignored) {}
        return map;
    }

    private static void writeCache(String key, Map<String, String[]> map) {
        File file = cacheFile();
        File tmp = new File(file.getPath() + ".tmp");
        try {
            file.getParentFile().mkdirs();
            try (FileWriter w = new FileWriter(tmp)) {
                w.write(key + "\n");
                for (Map.Entry<String, String[]> e : map.entrySet()) {
                    w.write(e.getKey() + " " + e.getValue()[0] + " " + e.getValue()[1] + "\n");
                }
            }
            if (!tmp.renameTo(file)) tmp.delete();
        } catch (Throwable t) {
            log("[!] Cannot write flag cache: " + t);
        }
    }

    // Returns {className, method} of the boolean getter for a flag id.
    private static String[] scanFlag(String apkPath, String flagId) {
        try {
            DexScanner.ScanResult result = DexScanner.scanApkForFlag(apkPath, flagId);
            if (result == null) {
                log("[X] Flag " + flagId + " not found in " + apkPath);
                return null;
            }
            log("[*] DexScanner result: " + result);

            String hookName = result.getterMethod;
            if (hookName == null && "Z".equals(result.flagReturnType)) {
                // Newer GMS inlines the getter: the flag method itself returns boolean.
                hookName = result.flagMethod;
            }
            if (result.className == null || hookName == null) {
                log("[X] No hookable boolean method for flag " + flagId);
                return null;
            }
            return new String[]{result.className, hookName};
        } catch (Throwable t) {
            log("[X] Scanning for flag " + flagId + " failed: " + t);
            return null;
        }
    }

    private static boolean hookMethod(ClassLoader gmsLoader, String apkPath, String flagId,
                                      String className, String hookName) {
        try {
            ClassLoader targetLoader = resolveClassLoader(gmsLoader, apkPath, className);
            Method targetMethod = targetLoader.loadClass(className).getDeclaredMethod(hookName);
            if (targetMethod.getReturnType() != boolean.class) return false;
            targetMethod.setAccessible(true);
            Method callback = Hooker.class.getDeclaredMethod("forceTrue", Object[].class);
            callback.setAccessible(true);

            if (nativeHook(targetMethod, callback, HOOKER)) {
                log("[OK] Flag " + flagId + " forced true via " + className + "." + hookName + "()");
                return true;
            }
            log("[X] nativeHook failed for flag " + flagId);
        } catch (Throwable t) {
            log("[X] Hooking flag " + flagId + " failed: " + t);
        }
        return false;
    }

    private static ClassLoader resolveClassLoader(ClassLoader initialLoader, String apkPath, String testClassName) {
        if (initialLoader != null) {
            try {
                initialLoader.loadClass(testClassName);
                return initialLoader;
            } catch (Throwable ignored) {}
        }
        ClassLoader ccl = Thread.currentThread().getContextClassLoader();
        if (ccl != null) {
            try {
                ccl.loadClass(testClassName);
                return ccl;
            } catch (Throwable ignored) {}
        }
        try {
            Class<?> pclClass = Class.forName("dalvik.system.PathClassLoader");
            return (ClassLoader) pclClass.getConstructor(String.class, ClassLoader.class)
                    .newInstance(apkPath, ClassLoader.getSystemClassLoader());
        } catch (Throwable ignored) {}
        return initialLoader != null ? initialLoader : ClassLoader.getSystemClassLoader();
    }

    private static String findGmsBaseApk(ClassLoader gmsLoader) {
        if (gmsLoader != null) {
            Matcher m = Pattern.compile("(/[^\"\\s:;\\]]+/com\\.google\\.android\\.gms[^\"\\s:;\\]]*/base\\.apk)")
                    .matcher(gmsLoader.toString());
            if (m.find() && new File(m.group(1)).exists()) return m.group(1);
            try {
                Field pathListField = Class.forName("dalvik.system.BaseDexClassLoader").getDeclaredField("pathList");
                pathListField.setAccessible(true);
                Object pathList = pathListField.get(gmsLoader);
                Field dexElementsField = pathList.getClass().getDeclaredField("dexElements");
                dexElementsField.setAccessible(true);
                for (Object element : (Object[]) dexElementsField.get(pathList)) {
                    Field fileField = element.getClass().getDeclaredField("file");
                    fileField.setAccessible(true);
                    File f = (File) fileField.get(element);
                    if (f != null && f.getAbsolutePath().contains("com.google.android.gms")
                            && f.getName().equals("base.apk") && f.exists()) {
                        return f.getAbsolutePath();
                    }
                }
            } catch (Throwable ignored) {}
        }
        try (BufferedReader br = new BufferedReader(new FileReader("/proc/self/maps"))) {
            String line;
            while ((line = br.readLine()) != null) {
                int idx = line.indexOf('/');
                if (idx != -1 && line.contains("com.google.android.gms") && line.contains("base.apk")) {
                    File f = new File(line.substring(idx).trim());
                    if (f.exists()) return f.getAbsolutePath();
                }
            }
        } catch (Throwable ignored) {}
        return null;
    }

    private static void log(String msg) {
        try {
            Class.forName("android.util.Log").getDeclaredMethod("i", String.class, String.class)
                    .invoke(null, TAG, msg);
        } catch (Throwable ignored) {}
    }
}
