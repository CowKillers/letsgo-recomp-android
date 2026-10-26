package com.letsgorecomp;

// The launcher speaks the phone's language. The words it always showed were Portuguese and stay in
// the code as the default; apk/res/values-en|fr/strings.xml say the same things in English and in
// French, and t("play", "Jogar") returns whichever the system asks for. A key missing from those
// files changes nothing: the Portuguese string comes back.
class Txt {
    private static android.content.res.Resources res;
    private static String pkg = "com.letsgorecomp";

    static void init(android.content.Context ctx) {
        try {
            pkg = ctx.getPackageName();
            android.content.res.Configuration c =
                new android.content.res.Configuration(ctx.getResources().getConfiguration());
            c.setLocale(c.locale);  // (the phone's language, on a copy that keeps its other settings)
            res = ctx.getApplicationContext().createConfigurationContext(c).getResources();
        } catch (Exception e) { res = null; }
    }

    /** `key' in the phone's language; `pt', the string the code always had, when there is none. */
    static String t(String key, String pt) {
        if (res != null) {
            try {
                int id = res.getIdentifier(key, "string", pkg);
                if (id != 0) { String s = res.getString(id); if (s != null && !s.isEmpty()) return s; }
            } catch (Exception ignored) {}
        }
        return pt;
    }
}
