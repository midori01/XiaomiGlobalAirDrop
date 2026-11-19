package com.midori.xga;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public class DexScanner {

    public static class ScanResult {
        public final String className;          // e.g. "kbhx"
        public final String flagMethod;         // e.g. "cT"
        public final String getterMethod;       // e.g. "aL"
        public final String flagInterface;      // e.g. "kbhw"
        public final String flagReturnType;     // e.g. "hfuf"
        public final String registryClass;      // e.g. "hfvp"
        public final String registryMethod;     // e.g. "i"
        public final String flagEvalMethod;     // e.g. "lw"

        public ScanResult(String className, String flagMethod, String getterMethod,
                          String flagInterface, String flagReturnType,
                          String registryClass, String registryMethod, String flagEvalMethod) {
            this.className = className;
            this.flagMethod = flagMethod;
            this.getterMethod = getterMethod;
            this.flagInterface = flagInterface;
            this.flagReturnType = flagReturnType;
            this.registryClass = registryClass;
            this.registryMethod = registryMethod;
            this.flagEvalMethod = flagEvalMethod;
        }

        @Override
        public String toString() {
            return "ScanResult{" +
                    "class='" + className + '\'' +
                    ", flagMethod='" + flagMethod + '\'' +
                    ", getterMethod='" + getterMethod + '\'' +
                    ", flagInterface='" + flagInterface + '\'' +
                    ", flagReturnType='" + flagReturnType + '\'' +
                    ", registryClass='" + registryClass + '\'' +
                    ", registryMethod='" + registryMethod + '\'' +
                    ", flagEvalMethod='" + flagEvalMethod + '\'' +
                    '}';
        }
    }

    public static ScanResult scanApkForFlag(String apkPath, String targetFlagId) {
        if (apkPath == null || targetFlagId == null) return null;
        try (ZipFile zipFile = new ZipFile(apkPath)) {
            byte[] targetBytes = targetFlagId.getBytes(StandardCharsets.UTF_8);
            Enumeration<? extends ZipEntry> entries = zipFile.entries();
            while (entries.hasMoreElements()) {
                ZipEntry entry = entries.nextElement();
                String name = entry.getName();
                if (!name.endsWith(".dex")) continue;

                // Fast check if DEX stream contains flag bytes
                byte[] dexData = readZipEntry(zipFile, entry);
                if (dexData == null || !containsBytes(dexData, targetBytes)) continue;

                ScanResult result = scanDex(dexData, targetFlagId);
                if (result != null) return result;
            }
        } catch (Throwable ignored) {
        }
        return null;
    }

    private static byte[] readZipEntry(ZipFile zf, ZipEntry entry) {
        try (InputStream is = zf.getInputStream(entry); ByteArrayOutputStream bos = new ByteArrayOutputStream()) {
            byte[] buf = new byte[8192];
            int r;
            while ((r = is.read(buf)) != -1) bos.write(buf, 0, r);
            return bos.toByteArray();
        } catch (Throwable t) {
            return null;
        }
    }

    private static boolean containsBytes(byte[] data, byte[] target) {
        if (data == null || target == null || data.length < target.length) return false;
        outer:
        for (int i = 0; i <= data.length - target.length; i++) {
            for (int j = 0; j < target.length; j++) {
                if (data[i + j] != target[j]) continue outer;
            }
            return true;
        }
        return false;
    }

    private static ScanResult scanDex(byte[] data, String targetFlagId) {
        try {
            ByteBuffer buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
            int stringIdsSize = buf.getInt(56);
            int stringIdsOff = buf.getInt(60);
            int typeIdsSize = buf.getInt(64);
            int typeIdsOff = buf.getInt(68);
            int protoIdsSize = buf.getInt(72);
            int protoIdsOff = buf.getInt(76);
            int methodIdsSize = buf.getInt(88);
            int methodIdsOff = buf.getInt(92);
            int classDefsSize = buf.getInt(96);
            int classDefsOff = buf.getInt(100);

            // Find target string ID
            int targetStrIdx = -1;
            for (int sid = 0; sid < stringIdsSize; sid++) {
                int off = buf.getInt(stringIdsOff + sid * 4);
                String str = readMutf8String(data, off);
                if (targetFlagId.equals(str)) {
                    targetStrIdx = sid;
                    break;
                }
            }
            if (targetStrIdx == -1) return null;

            // Search class definitions for const-string (0x1a) referencing targetStrIdx
            for (int cid = 0; cid < classDefsSize; cid++) {
                int cdefPos = classDefsOff + cid * 32;
                int typeIdx = buf.getInt(cdefPos);
                int interfacesOff = buf.getInt(cdefPos + 12);
                int classDataOff = buf.getInt(cdefPos + 24);
                if (classDataOff == 0) continue;

                String classDesc = getType(data, buf, typeIdsOff, stringIdsOff, typeIdx);
                int[] p = new int[]{classDataOff};
                int staticFieldsSize = readUleb128(data, p);
                int instanceFieldsSize = readUleb128(data, p);
                int directMethodsSize = readUleb128(data, p);
                int virtualMethodsSize = readUleb128(data, p);

                for (int i = 0; i < (staticFieldsSize + instanceFieldsSize); i++) {
                    readUleb128(data, p);
                    readUleb128(data, p);
                }

                int[] matchedMid = new int[]{-1};
                int[] flagCodeOff = new int[]{-1};

                scanMethodsForConstString(data, buf, p, directMethodsSize, targetStrIdx, matchedMid, flagCodeOff);
                if (matchedMid[0] == -1) {
                    scanMethodsForConstString(data, buf, p, virtualMethodsSize, targetStrIdx, matchedMid, flagCodeOff);
                }

                if (matchedMid[0] != -1) {
                    int flagMid = matchedMid[0];
                    String flagMethodName = getMethodName(data, buf, methodIdsOff, stringIdsOff, flagMid);
                    String flagReturnType = getMethodReturnType(data, buf, methodIdsOff, protoIdsOff, typeIdsOff, stringIdsOff, flagMid);
                    String cleanClassName = cleanType(classDesc);

                    // 1. Interface
                    String flagInterface = null;
                    if (interfacesOff != 0) {
                        int ifaceSize = buf.getInt(interfacesOff);
                        if (ifaceSize > 0) {
                            int ifaceTypeIdx = buf.getShort(interfacesOff + 4) & 0xFFFF;
                            flagInterface = cleanType(getType(data, buf, typeIdsOff, stringIdsOff, ifaceTypeIdx));
                        }
                    }

                    // 2. Discover Phenotype registry call from flagMethod bytecode
                    String registryClass = null;
                    String registryMethod = null;
                    if (flagCodeOff[0] != -1) {
                        int isize = buf.getInt(flagCodeOff[0] + 12);
                        int istart = flagCodeOff[0] + 16;
                        int iend = istart + isize * 2;
                        for (int ip = istart; ip <= iend - 4; ip += 2) {
                            int op = data[ip] & 0xFF;
                            if (isInvokeOp(op)) {
                                int cmid = (data[ip + 2] & 0xFF) | ((data[ip + 3] & 0xFF) << 8);
                                String cmClass = getMethodDeclaringClass(data, buf, methodIdsOff, typeIdsOff, stringIdsOff, cmid);
                                if (!cleanClassName.equals(cmClass)) {
                                    registryClass = cmClass;
                                    registryMethod = getMethodName(data, buf, methodIdsOff, stringIdsOff, cmid);
                                    break;
                                }
                            }
                        }
                    }

                    // 3. Discover getterMethod (caller of flagMid) and flagEvalMethod
                    String[] callerResult = new String[2]; // [0] = getterMethod, [1] = flagEvalMethod
                    findCallerAndEvalInClass(data, buf, classDataOff, flagMid, flagReturnType,
                            methodIdsOff, protoIdsOff, typeIdsOff, stringIdsOff, callerResult);

                    return new ScanResult(cleanClassName, flagMethodName, callerResult[0],
                            flagInterface, flagReturnType, registryClass, registryMethod, callerResult[1]);
                }
            }
        } catch (Throwable ignored) {
        }
        return null;
    }

    private static boolean isInvokeOp(int op) {
        return op == 0x6E || op == 0x6F || op == 0x70 || op == 0x71 || op == 0x72
                || (op >= 0x74 && op <= 0x78);
    }

    private static void scanMethodsForConstString(byte[] data, ByteBuffer buf, int[] p, int count,
                                                 int targetStrIdx, int[] matchedMid, int[] matchedCodeOff) {
        int lastMid = 0;
        for (int i = 0; i < count; i++) {
            lastMid += readUleb128(data, p);
            readUleb128(data, p); // access
            int codeOff = readUleb128(data, p);
            if (codeOff != 0 && matchedMid[0] == -1) {
                int insnsSize = buf.getInt(codeOff + 12);
                int insnsStart = codeOff + 16;
                int insnsEnd = insnsStart + insnsSize * 2;
                for (int ip = insnsStart; ip <= insnsEnd - 4; ip += 2) {
                    int op = data[ip] & 0xFF;
                    if (op == 0x1A) { // const-string
                        int sid = (data[ip + 2] & 0xFF) | ((data[ip + 3] & 0xFF) << 8);
                        if (sid == targetStrIdx) {
                            matchedMid[0] = lastMid;
                            matchedCodeOff[0] = codeOff;
                            break;
                        }
                    }
                }
            }
        }
    }

    private static void findCallerAndEvalInClass(byte[] data, ByteBuffer buf, int classDataOff,
                                                 int targetMid, String flagReturnType,
                                                 int methodIdsOff, int protoIdsOff, int typeIdsOff, int stringIdsOff,
                                                 String[] outResult) {
        try {
            int[] p = new int[]{classDataOff};
            int sCount = readUleb128(data, p);
            int iCount = readUleb128(data, p);
            int dCount = readUleb128(data, p);
            int vCount = readUleb128(data, p);
            for (int i = 0; i < (sCount + iCount); i++) {
                readUleb128(data, p);
                readUleb128(data, p);
            }

            // Direct methods scan
            int lastMid = 0;
            for (int i = 0; i < dCount; i++) {
                lastMid += readUleb128(data, p);
                readUleb128(data, p);
                int codeOff = readUleb128(data, p);
                if (outResult[0] == null && codeOff != 0) {
                    checkMethodCalls(data, buf, codeOff, lastMid, targetMid, flagReturnType,
                            methodIdsOff, protoIdsOff, typeIdsOff, stringIdsOff, outResult);
                }
            }

            // Virtual methods scan (MUST reset lastMid = 0 in DEX specification)
            lastMid = 0;
            for (int i = 0; i < vCount; i++) {
                lastMid += readUleb128(data, p);
                readUleb128(data, p);
                int codeOff = readUleb128(data, p);
                if (outResult[0] == null && codeOff != 0) {
                    checkMethodCalls(data, buf, codeOff, lastMid, targetMid, flagReturnType,
                            methodIdsOff, protoIdsOff, typeIdsOff, stringIdsOff, outResult);
                }
            }
        } catch (Throwable ignored) {
        }
    }

    private static void checkMethodCalls(byte[] data, ByteBuffer buf, int codeOff, int currentMid,
                                         int targetMid, String flagReturnType,
                                         int methodIdsOff, int protoIdsOff, int typeIdsOff, int stringIdsOff,
                                         String[] outResult) {
        int insnsSize = buf.getInt(codeOff + 12);
        int insnsStart = codeOff + 16;
        int insnsEnd = insnsStart + insnsSize * 2;
        for (int ip = insnsStart; ip <= insnsEnd - 4; ip += 2) {
            int op = data[ip] & 0xFF;
            if (isInvokeOp(op)) {
                int calledMid = (data[ip + 2] & 0xFF) | ((data[ip + 3] & 0xFF) << 8);
                if (calledMid == targetMid) {
                    outResult[0] = getMethodName(data, buf, methodIdsOff, stringIdsOff, currentMid);

                    // Scan subsequent instructions in this method to find evaluation call on flagReturnType
                    for (int j = ip + 2; j <= insnsEnd - 4; j += 2) {
                        int op2 = data[j] & 0xFF;
                        if (op2 == 0x6E || op2 == 0x72 || op2 == 0x74 || op2 == 0x78) { // invoke-virtual / invoke-interface
                            int evalMid = (data[j + 2] & 0xFF) | ((data[j + 3] & 0xFF) << 8);
                            String evalClass = getMethodDeclaringClass(data, buf, methodIdsOff, typeIdsOff, stringIdsOff, evalMid);
                            if (flagReturnType != null && flagReturnType.equals(evalClass)) {
                                outResult[1] = getMethodName(data, buf, methodIdsOff, stringIdsOff, evalMid);
                                break;
                            }
                        }
                    }
                    return;
                }
            }
        }
    }

    private static String cleanType(String typeDesc) {
        if (typeDesc != null && typeDesc.startsWith("L") && typeDesc.endsWith(";")) {
            return typeDesc.substring(1, typeDesc.length() - 1);
        }
        return typeDesc;
    }

    private static String readMutf8String(byte[] data, int offset) {
        int p = offset;
        int len = 0;
        int shift = 0;
        while (true) {
            byte b = data[p++];
            len |= (b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
        }
        return new String(data, p, len, StandardCharsets.UTF_8);
    }

    private static String getType(byte[] data, ByteBuffer buf, int typeIdsOff, int stringIdsOff, int typeIdx) {
        int strIdx = buf.getInt(typeIdsOff + typeIdx * 4);
        int strOff = buf.getInt(stringIdsOff + strIdx * 4);
        return readMutf8String(data, strOff);
    }

    private static String getMethodName(byte[] data, ByteBuffer buf, int methodIdsOff, int stringIdsOff, int methodIdx) {
        int nameIdx = buf.getInt(methodIdsOff + methodIdx * 8 + 4);
        int strOff = buf.getInt(stringIdsOff + nameIdx * 4);
        return readMutf8String(data, strOff);
    }

    private static String getMethodDeclaringClass(byte[] data, ByteBuffer buf, int methodIdsOff, int typeIdsOff, int stringIdsOff, int methodIdx) {
        int classIdx = buf.getShort(methodIdsOff + methodIdx * 8) & 0xFFFF;
        return cleanType(getType(data, buf, typeIdsOff, stringIdsOff, classIdx));
    }

    private static String getMethodReturnType(byte[] data, ByteBuffer buf, int methodIdsOff, int protoIdsOff, int typeIdsOff, int stringIdsOff, int methodIdx) {
        int protoIdx = buf.getShort(methodIdsOff + methodIdx * 8 + 2) & 0xFFFF;
        int returnTypeIdx = buf.getInt(protoIdsOff + protoIdx * 12 + 4);
        return cleanType(getType(data, buf, typeIdsOff, stringIdsOff, returnTypeIdx));
    }

    private static int readUleb128(byte[] data, int[] p) {
        int res = 0;
        int shift = 0;
        while (true) {
            byte b = data[p[0]++];
            res |= (b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
        }
        return res;
    }
}
