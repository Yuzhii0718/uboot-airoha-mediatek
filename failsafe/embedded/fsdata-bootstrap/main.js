/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * This file is part of the project bl-mt798x-dhcpd
 * You may not use, copy, modify or distribute this file except in compliance with the license agreement.
 */

// Project/author constants (centralized for reuse)
const AUTHOR_HANDLE = "Yuzhii0718";
const AUTHOR_DISPLAY = "💡Yuzhii";
const UBOOT_VERSION = 'UBOOT-AIROHA-2026.10';
const PROJECT_REPO_DISPLAY = "Yuzhii0718/uboot-airoha";
const GITHUB_USER_URL = "https://github.com/Yuzhii0718/";
const PROJECT_REPO_URL = "https://github.com/Yuzhii0718/uboot-airoha";

// Single global state container (defined eagerly so early helpers can read it).
var APP_STATE = {
    lang: "en",
    theme: "auto",
    page: "",
};

/*
 * Languages can be trimmed at build time (see the WEBUI_FAILSAFE_I18N_*
 * Kconfig options), so the switcher must only offer what is really
 * embedded in I18N - otherwise picking a stripped language would
 * silently fall back to English.
 */
const LANG_ORDER = ["en", "zh-cn", "ru"];
const LANG_LABELS = { en: "English", "zh-cn": "简体中文", ru: "Русский" };

function availableLangs() {
    if (!isI18nAvailable()) return [];
    return LANG_ORDER.filter(function (code) { return !!I18N[code]; });
}

function firstAvailableLang() {
    const availableLanguageList = availableLangs();
    return availableLanguageList.length ? availableLanguageList[0] : "en";
}

function resolveLang(languageCode) {
    return availableLangs().indexOf(languageCode) >= 0
        ? languageCode
        : firstAvailableLang();
}

function normalizeLang(input) {
    if (!input) return firstAvailableLang();
    const lowerCaseLanguage = String(input).toLowerCase();
    if (lowerCaseLanguage.indexOf("zh") === 0) return resolveLang("zh-cn");
    if (lowerCaseLanguage.indexOf("ru") === 0 || lowerCaseLanguage.indexOf("be") === 0 || lowerCaseLanguage.indexOf("uk") === 0) return resolveLang("ru");
    return resolveLang("en");
}

function detectLang() {
    try {
        const storedLang = localStorage.getItem("lang");
        if (storedLang) return normalizeLang(storedLang);
    } catch { /* ignore */ }
    const candidates = navigator.languages?.length
        ? navigator.languages
        : (navigator.language ? [navigator.language] : []);
    return normalizeLang(candidates[0]);
}

function detectTheme() {
    try {
        return localStorage.getItem("theme") ?? "auto";
    } catch {
        return "auto";
    }
}

function normalizeThemeMode(input) {
    if (!input) return "auto";
    const normalizedMode = String(input).toLowerCase().trim();
    return normalizedMode === "light" || normalizedMode === "dark" || normalizedMode === "auto" ? normalizedMode : "auto";
}

function isI18nAvailable() {
    return typeof I18N !== "undefined" && I18N;
}

function isI18nEnabled() {
    return APP_STATE.i18nEnabled !== false;
}

function t(key, fallback) {
    const languageCode = APP_STATE.lang || "en";
    const defaultValue = fallback !== undefined ? fallback : key;
    if (!isI18nEnabled() || !isI18nAvailable()) return defaultValue;
    return I18N[languageCode]?.[key] ?? I18N.en?.[key] ?? defaultValue;
}

function applyI18n(rootNode) {
    const scope = rootNode || document;
    const enabled = isI18nEnabled() && isI18nAvailable();

    for (const node of scope.querySelectorAll("[data-i18n]")) {
        const key = node.getAttribute("data-i18n");
        if (!node.hasAttribute("data-i18n-fallback")) {
            node.setAttribute("data-i18n-fallback", node.textContent || "");
        }
        const fallback = node.getAttribute("data-i18n-fallback") || "";
        node.textContent = enabled ? t(key, fallback) : fallback;
    }

    for (const node of scope.querySelectorAll("[data-i18n-html]")) {
        const key = node.getAttribute("data-i18n-html");
        if (!node.hasAttribute("data-i18n-html-fallback")) {
            node.setAttribute("data-i18n-html-fallback", node.innerHTML || "");
        }
        const fallback = node.getAttribute("data-i18n-html-fallback") || "";
        node.innerHTML = enabled ? t(key, fallback) : fallback;
    }

    for (const node of scope.querySelectorAll("[data-i18n-attr]")) {
        const spec = node.getAttribute("data-i18n-attr");
        if (!spec) continue;
        const [attrName, ...keyParts] = spec.split(":");
        if (!attrName || keyParts.length === 0) continue;
        const key = keyParts.join(":");
        const fallbackKey = `data-i18n-attr-fallback-${attrName}`;
        if (!node.hasAttribute(fallbackKey)) {
            node.setAttribute(fallbackKey, node.getAttribute(attrName) || "");
        }
        const fallback = node.getAttribute(fallbackKey) || "";
        node.setAttribute(attrName, enabled ? t(key, fallback) : fallback);
    }
}

function setLang(language) {
    APP_STATE.lang = normalizeLang(language);
    try {
        localStorage.setItem("lang", APP_STATE.lang);
    } catch { /* ignore */ }
    applyI18n(document);
    /* The translated hint carries the placeholders of the current FIP
     * size again, so they are filled once more after every re-render. */
    updateFipMaxSizeLabels();
    if (typeof renderSysInfo === "function") renderSysInfo();
    updateDocumentTitle();
}

function updateThemeSelect() {
    const themeSelect = document.getElementById("theme_select");
    if (!themeSelect) return;
    themeSelect.value = APP_STATE.theme || "auto";
}

function setTheme(themeMode, options = {}) {
    const { persistLocal = true, persistEnv = false, silent = false } = options;
    APP_STATE.theme = normalizeThemeMode(themeMode || "auto");

    if (persistLocal) {
        try { localStorage.setItem("theme", APP_STATE.theme); }
        catch { /* ignore */ }
    }

    const rootElement = document.documentElement;
    if (typeof window.__failsafeThemeApplyMode === "function") {
        window.__failsafeThemeApplyMode(APP_STATE.theme, { silent });
    } else if (APP_STATE.theme === "auto") {
        rootElement.removeAttribute("data-theme");
    } else {
        rootElement.setAttribute("data-theme", APP_STATE.theme);
    }

    updateThemeSelect();
    if (persistEnv) saveThemeMode(APP_STATE.theme);
}

const THEME_COLOR_ENV_KEY = "failsafe_theme_color";
const THEME_COLOR_CACHE_KEY = "failsafe_theme_color_cache";
const THEME_COLOR_RAINBOW = "rainbow";
const ACCENT_PRESETS = ["#2563eb", "#0ea5e9", "#14b8a6", "#10b981", "#f59e0b", "#ef4444", "#ec4899", "#a855f7"];
const THEME_MODE_ENV_KEY = "failsafe_theme_mode";
const THEME_DARK_VARIANT_ENV_KEY = "failsafe_theme_dark_variant";
const THEME_DARK_VARIANT_CACHE_KEY = "failsafe_theme_dark_variant_cache";

function normalizeDarkVariant(input) {
    if (input == null) return "";
    const value = String(input).trim().toLowerCase();
    return value === "amoled" ? "amoled" : "";
}

const HEX3_RE = /^[0-9a-fA-F]{3}$/;
const HEX6_RE = /^[0-9a-fA-F]{6}$/;

function normalizeHexColor(input) {
    if (input == null) return null;
    let value = String(input).trim();
    if (!value) return null;
    if (value[0] === "#") value = value.slice(1);
    if (!HEX3_RE.test(value) && !HEX6_RE.test(value)) return null;
    const hex = value.length === 3
        ? `#${value[0]}${value[0]}${value[1]}${value[1]}${value[2]}${value[2]}`
        : `#${value}`;
    return hex.toLowerCase();
}

function hexToRgb(hex) {
    const normalizedHex = normalizeHexColor(hex);
    if (!normalizedHex) return null;
    return {
        r: parseInt(normalizedHex.slice(1, 3), 16),
        g: parseInt(normalizedHex.slice(3, 5), 16),
        b: parseInt(normalizedHex.slice(5, 7), 16),
    };
}

function pickRandomPreset() {
    return ACCENT_PRESETS[Math.floor(Math.random() * ACCENT_PRESETS.length)];
}

function applyAccentVars(color) {
    if (color === THEME_COLOR_RAINBOW) {
        color = pickRandomPreset();
    }
    const normalizedColor = normalizeHexColor(color);
    if (!normalizedColor) return false;
    const rgb = hexToRgb(normalizedColor);
    if (!rgb) return false;

    const rootStyle = document.documentElement.style;
    rootStyle.setProperty("--primary", normalizedColor);
    rootStyle.setProperty("--primary-rgb", `${rgb.r}, ${rgb.g}, ${rgb.b}`);
    rootStyle.setProperty("--primary-2", blendColor(normalizedColor, "#ffffff", 0.28));
    ensureThemeColorMeta(normalizedColor);
    return true;
}

function blendColor(sourceHex, targetHex, ratio) {
    const a = hexToRgb(sourceHex);
    const b = hexToRgb(targetHex);
    if (!a || !b) return sourceHex;
    const mix = (x, y) => Math.round(x + (y - x) * ratio).toString(16).padStart(2, "0");
    return `#${mix(a.r, b.r)}${mix(a.g, b.g)}${mix(a.b, b.b)}`;
}

function ensureThemeColorMeta(color) {
    if (!color) return;
    let meta = document.querySelector("meta[name='theme-color']");
    if (!meta) {
        meta = document.createElement("meta");
        meta.setAttribute("name", "theme-color");
        document.head?.appendChild(meta);
    }
    meta.setAttribute("content", color);
}

function updateAccentControls(color) {
    const isRainbow = color === THEME_COLOR_RAINBOW;
    const normalizedColor = isRainbow ? null : normalizeHexColor(color);
    const colorPicker = document.getElementById("accent_color_picker");
    const colorInput  = document.getElementById("accent_color_input");

    if (colorPicker) {
        if (isRainbow) colorPicker.value = "#000000";
        else if (normalizedColor) colorPicker.value = normalizedColor;
    }
    if (colorInput) {
        if (isRainbow) colorInput.value = "";
        else if (normalizedColor) colorInput.value = normalizedColor;
    }

    for (const swatch of document.querySelectorAll(".color-swatch")) {
        const presetColor = String(swatch.dataset?.color ?? "").toLowerCase();
        swatch.classList.toggle("active", !!normalizedColor && presetColor === normalizedColor);
    }

    /* rainbow swatch active state */
    const rainbowSwatch = document.querySelector(".color-swatch-rainbow");
    if (rainbowSwatch) {
        rainbowSwatch.classList.toggle("active", isRainbow);
    }
}

function applyAccentColor(color) {
    const isApplied = applyAccentVars(color);
    if (!isApplied) return false;
    /* When rainbow is selected, update controls with the original raw value
     * so the rainbow swatch highlights correctly. */
    updateAccentControls(color);
    return true;
}

try {
    const cachedColor = localStorage.getItem(THEME_COLOR_CACHE_KEY);
    if (cachedColor) {
        if (cachedColor === THEME_COLOR_RAINBOW) {
            applyAccentVars(pickRandomPreset());
        } else {
            applyAccentVars(cachedColor);
        }
    }
} catch { /* ignore */ }

async function saveThemeColor(color) {
    if (color === THEME_COLOR_RAINBOW) {
        try { localStorage.setItem(THEME_COLOR_CACHE_KEY, THEME_COLOR_RAINBOW); }
        catch { /* ignore */ }
        try {
            const formData = new FormData();
            formData.append("color", THEME_COLOR_RAINBOW);
            await fetch("/theme/set", { method: "POST", body: formData });
        } catch { /* network errors silently dropped */ }
        return;
    }
    const normalizedColor = normalizeHexColor(color);
    if (!normalizedColor) return;
    try { localStorage.setItem(THEME_COLOR_CACHE_KEY, normalizedColor); }
    catch { /* ignore */ }
    try {
        const formData = new FormData();
        formData.append("color", normalizedColor);
        await fetch("/theme/set", { method: "POST", body: formData });
    } catch { /* network errors silently dropped */ }
}

async function saveThemeMode(theme) {
    const normalizedMode = normalizeThemeMode(theme);
    try { localStorage.setItem("theme", normalizedMode); }
    catch { /* ignore */ }
    try {
        const formData = new FormData();
        formData.append("theme", normalizedMode);
        await fetch("/theme/set", { method: "POST", body: formData });
    } catch { /* network errors silently dropped */ }
}

async function loadThemeColor() {
    let currentColor = null;
    let loadedFromEnv = false;
    let isRainbow = false;
    try {
        const response = await fetch("/theme/get", { method: "GET" });
        if (response?.ok) {
            const payload = await response.json();
            if (payload?.color === THEME_COLOR_RAINBOW) {
                isRainbow = true;
                loadedFromEnv = true;
            } else {
                currentColor = normalizeHexColor(payload?.color);
                loadedFromEnv = !!currentColor;
            }
        }
    } catch { /* ignore */ }

    if (isRainbow && loadedFromEnv) {
        /* server says rainbow — trigger a re-randomize and apply */
        applyAccentColor(THEME_COLOR_RAINBOW);
        try { localStorage.setItem(THEME_COLOR_CACHE_KEY, THEME_COLOR_RAINBOW); }
        catch { /* ignore */ }
        return;
    }

    if (!currentColor) {
        try {
            const cssValue = getComputedStyle(document.documentElement).getPropertyValue("--primary") ?? "";
            currentColor = normalizeHexColor(cssValue.trim());
        } catch { /* ignore */ }
    }

    if (!currentColor) return;

    if (loadedFromEnv) {
        applyAccentColor(currentColor);
        try { localStorage.setItem(THEME_COLOR_CACHE_KEY, currentColor); }
        catch { /* ignore */ }
    }
    updateAccentControls(currentColor);
}

async function loadThemeMode() {
    let mode = null;
    try {
        const response = await fetch("/theme/get", { method: "GET" });
        if (response?.ok) {
            const payload = await response.json();
            if (payload?.theme) mode = normalizeThemeMode(payload.theme);
        }
    } catch { /* ignore */ }

    if (mode) setTheme(mode, { persistEnv: false, persistLocal: true, silent: true });
}

function updateDarkVariantControl(variant) {
    const select = document.getElementById("settings_dark_variant");
    if (!select) return;
    select.value = normalizeDarkVariant(variant);
}

function applyDarkVariant(variant, options = {}) {
    const { persistLocal = true, silent = false } = options;
    const normalized = normalizeDarkVariant(variant);

    if (persistLocal) {
        try {
            if (normalized) localStorage.setItem(THEME_DARK_VARIANT_CACHE_KEY, normalized);
            else localStorage.removeItem(THEME_DARK_VARIANT_CACHE_KEY);
        } catch { /* ignore */ }
    }

    if (typeof window.__failsafeThemeApplyDarkVariant === "function") {
        window.__failsafeThemeApplyDarkVariant(normalized, { silent });
    } else {
        const root = document.documentElement;
        if (normalized) root.setAttribute("data-theme-dark", normalized);
        else root.removeAttribute("data-theme-dark");
    }

    updateDarkVariantControl(normalized);
}

async function saveDarkVariant(variant) {
    const normalized = normalizeDarkVariant(variant);
    /* server expects "standard" or empty to clear; we send "standard" so the
     * intent is explicit in transit, the backend turns it back into unset */
    const wire = normalized || "standard";
    try {
        const formData = new FormData();
        formData.append("dark_variant", wire);
        await fetch("/theme/set", { method: "POST", body: formData });
    } catch { /* network errors silently dropped */ }
}

async function loadDarkVariant() {
    let variant = null;
    try {
        const response = await fetch("/theme/get", { method: "GET" });
        if (response?.ok) {
            const payload = await response.json();
            variant = normalizeDarkVariant(payload?.dark_variant);
        }
    } catch { /* ignore */ }

    /* always update UI even when env-side is empty (need to clear stale select) */
    applyDarkVariant(variant ?? "", { persistLocal: true, silent: true });
}

function appendAccentControls(container) {
    if (!container) return;

    const row = document.createElement("div");
    row.className = "control-row control-row-color";

    const accentLabel = document.createElement("div");
    accentLabel.setAttribute("data-i18n", "control.accent");
    accentLabel.textContent = t("control.accent");
    row.appendChild(accentLabel);

    const picker = document.createElement("div");
    picker.className = "color-picker";

    const presets = document.createElement("div");
    presets.className = "color-presets";
    for (const presetColor of ACCENT_PRESETS) {
        const swatchButton = document.createElement("button");
        swatchButton.type = "button";
        swatchButton.className = "color-swatch";
        swatchButton.dataset.color = presetColor.toLowerCase();
        swatchButton.style.backgroundColor = presetColor;
        swatchButton.setAttribute("aria-label", `Accent ${presetColor}`);
        swatchButton.addEventListener("click", () => {
            applyAccentColor(presetColor);
            saveThemeColor(presetColor);
        });
        presets.appendChild(swatchButton);
    }

    /* rainbow swatch */
    const rainbowSwatch = document.createElement("button");
    rainbowSwatch.type = "button";
    rainbowSwatch.className = "color-swatch color-swatch-rainbow";
    rainbowSwatch.dataset.color = THEME_COLOR_RAINBOW;
    rainbowSwatch.setAttribute("data-i18n-attr", "title:theme.color.rainbow");
    rainbowSwatch.title = t("theme.color.rainbow");
    rainbowSwatch.setAttribute("aria-label", t("theme.color.rainbow"));
    rainbowSwatch.addEventListener("click", () => {
        applyAccentColor(THEME_COLOR_RAINBOW);
        saveThemeColor(THEME_COLOR_RAINBOW);
    });
    presets.appendChild(rainbowSwatch);

    const inputs = document.createElement("div");
    inputs.className = "color-inputs";

    const colorTextInput = document.createElement("input");
    colorTextInput.type = "text";
    colorTextInput.id = "accent_color_input";
    colorTextInput.setAttribute("data-i18n-attr", "placeholder:theme.color.placeholder");
    colorTextInput.placeholder = t("theme.color.placeholder");
    colorTextInput.addEventListener("change", () => {
        const normalizedColor = normalizeHexColor(colorTextInput.value);
        if (!normalizedColor) return;
        applyAccentColor(normalizedColor);
        saveThemeColor(normalizedColor);
    });

    const colorPicker = document.createElement("input");
    colorPicker.type = "color";
    colorPicker.id = "accent_color_picker";
    colorPicker.setAttribute("data-i18n-attr", "title:theme.color.custom");
    colorPicker.title = t("theme.color.custom");
    colorPicker.addEventListener("input", () => {
        applyAccentColor(colorPicker.value);
        saveThemeColor(colorPicker.value);
    });

    inputs.appendChild(colorTextInput);
    inputs.appendChild(colorPicker);

    picker.appendChild(presets);
    picker.appendChild(inputs);

    row.appendChild(picker);
    container.appendChild(row);
}

function ensureFavicon() {
    let link = document.querySelector("link[rel='icon']");
    if (!link) {
        link = document.createElement("link");
        link.setAttribute("rel", "icon");
        link.setAttribute("type", "image/svg+xml");
        link.setAttribute("href", "/favicon.svg");
        document.head?.appendChild(link);
    } else if (link.getAttribute("href") !== "/favicon.svg") {
        link.setAttribute("href", "/favicon.svg");
    }
}

const LOGO_CACHE_KEY = "failsafe_logo_dataurl";

function getLogoSrc() {
    try {
        const cached = sessionStorage.getItem(LOGO_CACHE_KEY);
        if (cached) return cached;
    } catch { /* sessionStorage unavailable */ }

    // Async populate cache for next page load
    fetch("/favicon.svg")
        .then((r) => r.ok ? r.blob() : Promise.reject())
        .then((blob) => {
            const reader = new FileReader();
            reader.onloadend = () => {
                if (typeof reader.result === "string" && reader.result.startsWith("data:image")) {
                    try { sessionStorage.setItem(LOGO_CACHE_KEY, reader.result); } catch {}
                }
            };
            reader.readAsDataURL(blob);
        })
        .catch(() => {});

    return "/favicon.svg";
}

function updateDocumentTitle() {
    if (!isI18nEnabled() || !isI18nAvailable() || !APP_STATE.page) return;

    const titleKey = `${APP_STATE.page}.title`;
    if (I18N[APP_STATE.lang]?.[titleKey]) {
        document.title = t(titleKey);
        return;
    }

    if (APP_STATE.page === "flashing") {
        document.title = t("flashing.title.in_progress");
    } else if (APP_STATE.page === "booting") {
        document.title = t("booting.title.in_progress");
    }
}

function ensureBranding() {
    const versionNode = document.getElementById("version");
    if (!versionNode) return;

    // Only remove sibling brand node if it exists (avoid DOM churn)
    const nextSibling = versionNode.nextElementSibling;
    if (nextSibling?.classList?.contains("brand")) {
        nextSibling.remove();
    }

    // Ensure an inline brand label exists (check first to avoid re-creation)
    if (!versionNode.querySelector?.(".brand-inline")) {
        const brandNode = document.createElement("span");
        brandNode.className = "brand-inline";
        brandNode.textContent = AUTHOR_DISPLAY;
        versionNode.append(" ", brandNode);
    }

    // Ensure project info block exists (don't duplicate)
    if (versionNode.querySelector?.("#project-info")) return;
    const projectInfo = document.createElement("div");
    projectInfo.id = "project-info";
    projectInfo.innerHTML = `You can find more infomation about this project: <a href="${PROJECT_REPO_URL}" target="_blank" rel="noopener">Github</a>`;
    versionNode.appendChild(projectInfo);
}

function ensureSidebar() {
    const createNavLink = (path, i18nKey, navId) => {
        const link = document.createElement("a");
        link.className = "nav-link";
        link.href = path;
        link.setAttribute("data-nav-id", navId);

        const iconSpan = document.createElement("span");
        iconSpan.className = "dot";
        link.appendChild(iconSpan);

        const labelSpan = document.createElement("span");
        labelSpan.setAttribute("data-i18n", i18nKey);
        labelSpan.textContent = t(i18nKey);
        link.appendChild(labelSpan);

        // Normalize and check active
        let normalizedPath = path;
        if (normalizedPath !== "/" && normalizedPath.charAt(0) !== "/") normalizedPath = "/" + normalizedPath;
        const isActive = normalizedPath === currentPath || (normalizedPath === "/" && (currentPath === "/" || currentPath === "/index.html"));
        if (isActive) link.classList.add("active");
        return link;
    };

    const sidebar = document.getElementById("sidebar");
    if (!sidebar) return;

    // Avoid re-rendering
    if (sidebar.getAttribute("data-rendered") === "1") return;
    sidebar.setAttribute("data-rendered", "1");

    // Prepare current path
    let currentPath = (location && location.pathname) ? location.pathname : "";
    if (currentPath === "") currentPath = "/";

    // Clear existing content
    sidebar.innerHTML = "";

    // Branding
    const brandContainer = document.createElement("div");
    brandContainer.className = "sidebar-brand";
    const brandLogo = document.createElement("img");
    brandLogo.className = "logo";
    brandLogo.src = getLogoSrc();
    brandLogo.alt = "";
    brandLogo.width = 28;
    brandLogo.height = 28;
    brandContainer.appendChild(brandLogo);
    const brandTitle = document.createElement("div");
    brandTitle.className = "title";
    brandTitle.setAttribute("data-i18n", "app.name");
    brandTitle.textContent = t("app.name");
    brandContainer.appendChild(brandTitle);

    const helpButton = document.createElement("button");
    helpButton.type = "button";
    helpButton.className = "help-btn";
    helpButton.title = t("help.tooltip", "About & Help");
    helpButton.setAttribute("data-i18n-attr", "title:help.tooltip");
    helpButton.setAttribute("aria-label", t("help.tooltip", "About & Help"));
    helpButton.innerHTML =
        '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
        '<circle cx="12" cy="12" r="10"></circle>' +
        '<path d="M9.5 9a2.5 2.5 0 1 1 4.5 1.5c-.6.5-1.5 1-1.5 2"></path>' +
        '<line x1="12" y1="17" x2="12.01" y2="17"></line>' +
        '</svg>';
    helpButton.addEventListener("click", openHelpModal);
    brandContainer.appendChild(helpButton);

    sidebar.appendChild(brandContainer);

    // Controls (language, theme, accent)
    const controlsContainer = document.createElement("div");
    controlsContainer.className = "sidebar-controls";

    const languageRow = document.createElement("div");
    languageRow.className = "control-row";
    const languageLabel = document.createElement("div");
    languageLabel.setAttribute("data-i18n", "control.language");
    languageLabel.textContent = t("control.language");
    languageRow.appendChild(languageLabel);

    const languageSelect = document.createElement("select");
    languageSelect.id = "lang_select";
    const availableLanguageList = availableLangs();
    languageSelect.innerHTML = availableLanguageList
        .map(function (code) {
            return '<option value="' + code + '">' + (LANG_LABELS[code] || code) + "</option>";
        })
        .join("");
    if (availableLanguageList.length < 2) {
        /* only one (or no) language embedded: nothing to switch */
        languageRow.hidden = true;
    } else {
        languageSelect.value = availableLanguageList.indexOf(APP_STATE.lang) >= 0
            ? APP_STATE.lang
            : availableLanguageList[0];
    }
    languageSelect.onchange = function () { setLang(this.value); };
    languageRow.appendChild(languageSelect);
    controlsContainer.appendChild(languageRow);

    const themeRow = document.createElement("div");
    themeRow.className = "control-row";
    const themeLabel = document.createElement("div");
    themeLabel.setAttribute("data-i18n", "control.theme");
    themeLabel.textContent = t("control.theme");
    themeRow.appendChild(themeLabel);

    const themeSelect = document.createElement("select");
    themeSelect.id = "theme_select";
    const autoOption = document.createElement("option");
    autoOption.value = "auto";
    autoOption.setAttribute("data-i18n", "theme.auto");
    autoOption.textContent = t("theme.auto");
    const lightOption = document.createElement("option");
    lightOption.value = "light";
    lightOption.setAttribute("data-i18n", "theme.light");
    lightOption.textContent = t("theme.light");
    const darkOption = document.createElement("option");
    darkOption.value = "dark";
    darkOption.setAttribute("data-i18n", "theme.dark");
    darkOption.textContent = t("theme.dark");
    themeSelect.appendChild(autoOption);
    themeSelect.appendChild(lightOption);
    themeSelect.appendChild(darkOption);
    themeSelect.value = APP_STATE.theme;
    themeSelect.onchange = function () { setTheme(this.value, { persistEnv: true, persistLocal: true }); };
    themeRow.appendChild(themeSelect);
    controlsContainer.appendChild(themeRow);

    sidebar.appendChild(controlsContainer);

    // Navigation
    const navContainer = document.createElement("div");
    navContainer.className = "nav";

    // Settings (placed before Basic section)
    const settingsLink = createNavLink("/settings.html", "nav.settings", "settings");
    settingsLink.style.display = "none";
    navContainer.appendChild(settingsLink);

    // Basic section
    const basicSection = document.createElement("div");
    basicSection.className = "nav-section";
    const basicTitle = document.createElement("div");
    basicTitle.className = "nav-section-title";
    basicTitle.setAttribute("data-i18n", "nav.basic");
    basicTitle.textContent = t("nav.basic");
    basicSection.appendChild(basicTitle);
    basicSection.appendChild(createNavLink("/", "nav.firmware", "firmware"));
    const ubootLink = createNavLink("/uboot.html", "nav.uboot", "uboot");
    ubootLink.style.display = "none";
    basicSection.appendChild(ubootLink);
    const bl2Link = createNavLink("/bl2.html", "nav.bl2", "bl2");
    bl2Link.style.display = "none";
    basicSection.appendChild(bl2Link);
    const chainloaderLink = createNavLink("/chainloader.html", "nav.chainloader", "chainloader");
    chainloaderLink.style.display = "none";
    basicSection.appendChild(chainloaderLink);
    const fipLink = createNavLink("/fip.html", "nav.fip", "fip");
    fipLink.style.display = "none";
    basicSection.appendChild(fipLink);
    navContainer.appendChild(basicSection);

    // Advanced section
    const advancedSection = document.createElement("div");
    advancedSection.className = "nav-section";
    const advancedTitle = document.createElement("div");
    advancedTitle.className = "nav-section-title";
    advancedTitle.setAttribute("data-i18n", "nav.advanced");
    advancedTitle.textContent = t("nav.advanced");
    advancedSection.appendChild(advancedTitle);
    const gptLink = createNavLink("/gpt.html", "nav.gpt", "gpt");
    gptLink.style.display = "none";
    advancedSection.appendChild(gptLink);
    const ubiLink = createNavLink("/ubi.html", "nav.ubi", "ubi");
    ubiLink.style.display = "none";
    advancedSection.appendChild(ubiLink);
    const flashLink = createNavLink("/flash.html", "nav.flash", "flash");
    flashLink.style.display = "none";
    advancedSection.appendChild(flashLink);
    const simgLink = createNavLink("/simg.html", "nav.simg", "simg");
    simgLink.style.display = "none";
    advancedSection.appendChild(simgLink);
    advancedSection.appendChild(createNavLink("/env.html", "nav.env", "env"));
    advancedSection.appendChild(createNavLink("/console.html", "nav.console", "console"));
    navContainer.appendChild(advancedSection);

    // System section
    const systemSection = document.createElement("div");
    systemSection.className = "nav-section";
    const systemTitle = document.createElement("div");
    systemTitle.className = "nav-section-title";
    systemTitle.setAttribute("data-i18n", "nav.system");
    systemTitle.textContent = t("nav.system");
    systemSection.appendChild(systemTitle);
    systemSection.appendChild(createNavLink("/initramfs.html", "nav.initramfs", "initramfs"));
    systemSection.appendChild(createNavLink("/reboot.html", "nav.reboot", "reboot"));
    navContainer.appendChild(systemSection);

    sidebar.appendChild(navContainer);

    applyI18n(sidebar);

    /*
     * Reveal the pages this firmware was built with while the sidebar is
     * built: the known list is applied inside this same task, before the
     * browser paints, so nothing appears or disappears afterwards (see
     * readKnownPages()).  applyNavVisibility() only refreshes a changed
     * list.
     */
    const knownPages = readKnownPages();

    if (knownPages) applyPageList(knownPages);

    applyNavVisibility(knownPages);

    attachSidebarScrollPersistence(navContainer);
}

function ensureHelpModal() {
    let backdrop = document.getElementById("help_modal_backdrop");
    if (backdrop) return backdrop;

    backdrop = document.createElement("div");
    backdrop.id = "help_modal_backdrop";
    backdrop.className = "help-modal-backdrop";

    const modal = document.createElement("div");
    modal.className = "help-modal";
    modal.setAttribute("role", "dialog");
    modal.setAttribute("aria-modal", "true");
    modal.setAttribute("aria-labelledby", "help_modal_title");

    const closeButton = document.createElement("button");
    closeButton.type = "button";
    closeButton.className = "help-modal-close";
    closeButton.setAttribute("data-i18n-attr", "aria-label:common.close");
    closeButton.setAttribute("aria-label", t("common.close", "Close"));
    closeButton.innerHTML = "&times;";
    closeButton.addEventListener("click", closeHelpModal);
    modal.appendChild(closeButton);

    const header = document.createElement("div");
    header.className = "help-modal-header";

    const logo = document.createElement("img");
    logo.className = "help-modal-logo";
    logo.src = getLogoSrc();
    logo.alt = "";
    header.appendChild(logo);

    const titles = document.createElement("div");
    const title = document.createElement("h2");
    title.id = "help_modal_title";
    title.className = "help-modal-title";
    title.setAttribute("data-i18n", "help.title");
    title.textContent = t("help.title", "About & Help");
    titles.appendChild(title);

    const subtitle = document.createElement("p");
    subtitle.className = "help-modal-subtitle";
    subtitle.setAttribute("data-i18n", "app.name");
    subtitle.textContent = t("app.name");
    titles.appendChild(subtitle);

    header.appendChild(titles);
    modal.appendChild(header);

    const body = document.createElement("div");
    body.className = "help-modal-body";

    const intro = document.createElement("p");
    intro.className = "help-modal-intro";
    intro.setAttribute("data-i18n", "help.intro");
    intro.textContent = t("help.intro");
    body.appendChild(intro);

    const info = document.createElement("dl");
    info.className = "help-modal-info";

    const authorDt = document.createElement("dt");
    authorDt.setAttribute("data-i18n", "help.author");
    authorDt.textContent = t("help.author", "Author");
    info.appendChild(authorDt);

    const authorDd = document.createElement("dd");
    const authorLink = document.createElement("a");
    authorLink.href = GITHUB_USER_URL;
    authorLink.target = "_blank";
    authorLink.rel = "noopener";
    authorLink.textContent = AUTHOR_DISPLAY;
    authorDd.appendChild(authorLink);
    info.appendChild(authorDd);

    const projectDt = document.createElement("dt");
    projectDt.setAttribute("data-i18n", "help.project");
    projectDt.textContent = t("help.project", "Project");
    info.appendChild(projectDt);

    const projectDd = document.createElement("dd");
    const projectLink = document.createElement("a");
    projectLink.href = PROJECT_REPO_URL;
    projectLink.target = "_blank";
    projectLink.rel = "noopener";
    projectLink.textContent = PROJECT_REPO_DISPLAY;
    projectDd.appendChild(projectLink);
    info.appendChild(projectDd);

    body.appendChild(info);
    modal.appendChild(body);

    backdrop.appendChild(modal);

    backdrop.addEventListener("click", (event) => {
        if (event.target === backdrop) closeHelpModal();
    });

    document.body.appendChild(backdrop);
    return backdrop;
}

function handleHelpModalKey(event) {
    if (event.key === "Escape") closeHelpModal();
}

function openHelpModal() {
    const backdrop = ensureHelpModal();
    applyI18n(backdrop);
    backdrop.classList.add("is-open");
    document.addEventListener("keydown", handleHelpModalKey);
}

function closeHelpModal() {
    const backdrop = document.getElementById("help_modal_backdrop");
    if (backdrop) backdrop.classList.remove("is-open");
    document.removeEventListener("keydown", handleHelpModalKey);
}

/*
 * RAM (XMODEM recovery) session warning.
 *
 * A bootloader that was fetched over the console by the previous boot stage
 * runs from DRAM only: see common/boot_mode.c and the "boot" field of
 * GET /sysinfo.  Nothing the user uploads through this UI reaches the flash
 * until it is written explicitly, so uploading only a firmware image - or
 * simply rebooting - leaves the device with a missing or mismatched
 * bootloader, which is exactly how a device bricks.
 *
 * GET /sysinfo reports three states: "ram", "flash" and "unknown".  Only
 * "ram" warns; a platform or a boot chain that cannot report the mode must
 * never produce a false alarm.
 *
 * Two things are shown: a banner on every page (a dismissed modal is too
 * easy to forget) and, on the entry page, a modal that cannot be missed.
 * The modal is remembered for the browser session, so moving between the
 * pages does not keep it in the way, while the banner stays.
 */
const BOOTMODE_MODAL_KEY = "failsafe_ram_modal_shown";

/* Bootloader pages of the builds this UI is used with, in the order they
 * belong together: a FIP layout needs the preloader (bl2) and the FIP
 * (bl31 + U-Boot) both, so the warning has to offer both instead of hiding
 * one behind the other.  The ids are the ones GET /ui/pages reports (see
 * failsafe/pages.c). */
const BOOTMODE_PAGE_ORDER = ["bl2", "fip", "uboot", "chainloader"];

function bootModeFromSysInfo() {
    return APP_STATE.sysinfo?.boot?.mode || "unknown";
}

function bootModeIsRam() {
    return bootModeFromSysInfo() === "ram";
}

/*
 * Bootloader pages this firmware really has, empty while the page list is
 * not known yet (it arrives with GET /ui/pages; applyPageList() re-runs the
 * warning when it shows up).
 */
function bootModeBootloaderPages() {
    const pages = readKnownPages();

    if (!Array.isArray(pages)) return [];

    return BOOTMODE_PAGE_ORDER.filter(function (pageId) {
        return pages.includes(pageId);
    });
}

/* One action per bootloader page of this build. */
function bootModeActionNodes(className) {
    return bootModeBootloaderPages().map(function (pageId) {
        const link = document.createElement("a");

        link.className = className;
        link.href = "/" + pageId + ".html";
        link.setAttribute("data-i18n", "ram.goto." + pageId);
        link.textContent = t("ram.goto." + pageId, pageId);

        return link;
    });
}

/* Label already translated by the caller: every node is tagged with its i18n
 * key so a language switch re-renders it through applyI18n(). */
function bootModeTextNode(tagName, className, key, fallback) {
    const node = document.createElement(tagName);

    node.className = className;
    node.setAttribute("data-i18n", key);
    node.textContent = t(key, fallback);

    return node;
}

function ensureBootModeBanner() {
    const main = document.querySelector(".main");
    let banner = document.getElementById("bootmode_banner");
    let actions;

    if (!main) return null;

    if (!banner) {
        banner = document.createElement("div");
        banner.id = "bootmode_banner";
        banner.className = "bootmode-banner";
        banner.setAttribute("role", "alert");
        banner.appendChild(bootModeTextNode("span", "bootmode-banner-tag",
            "ram.badge", "RAM"));
        banner.appendChild(bootModeTextNode("span", "bootmode-banner-text",
            "ram.banner", ""));

        actions = document.createElement("span");
        actions.id = "bootmode_banner_actions";
        actions.className = "bootmode-banner-actions";
        banner.appendChild(actions);

        main.insertBefore(banner, main.firstChild);
    } else {
        actions = document.getElementById("bootmode_banner_actions");
    }

    /* Rebuilt on every call: the bootloader actions follow the page list,
     * which may only arrive with GET /ui/pages (see applyPageList). */
    while (actions.firstChild) actions.removeChild(actions.firstChild);

    for (const action of bootModeActionNodes("bootmode-banner-link"))
        actions.appendChild(action);

    applyI18n(banner);

    return banner;
}

function removeBootModeBanner() {
    const banner = document.getElementById("bootmode_banner");

    if (banner) banner.remove();
}

function ensureBootModeModal() {
    let backdrop = document.getElementById("bootmode_modal_backdrop");

    if (backdrop) return backdrop;

    backdrop = document.createElement("div");
    backdrop.id = "bootmode_modal_backdrop";
    backdrop.className = "help-modal-backdrop";

    /* Same shell as the help modal (see ensureHelpModal), flagged as a
     * warning so it does not read as informational. */
    const modal = document.createElement("div");
    modal.className = "help-modal help-modal-warn";
    modal.setAttribute("role", "dialog");
    modal.setAttribute("aria-modal", "true");
    modal.setAttribute("aria-labelledby", "bootmode_modal_title");

    const closeButton = document.createElement("button");
    closeButton.type = "button";
    closeButton.className = "help-modal-close";
    closeButton.setAttribute("aria-label", t("common.close", "Close"));
    closeButton.innerHTML = "&times;";
    closeButton.addEventListener("click", closeBootModeModal);
    modal.appendChild(closeButton);

    const header = document.createElement("div");
    header.className = "help-modal-header";

    const titles = document.createElement("div");
    titles.appendChild(bootModeTextNode("h2", "help-modal-title",
        "ram.title", ""));
    titles.querySelector("h2").id = "bootmode_modal_title";
    titles.appendChild(bootModeTextNode("p", "help-modal-subtitle",
        "ram.subtitle", ""));
    header.appendChild(titles);
    modal.appendChild(header);

    const body = document.createElement("div");
    body.className = "help-modal-body";

    const intro = document.createElement("p");
    intro.className = "help-modal-intro";
    intro.setAttribute("data-i18n-html", "ram.body");
    intro.innerHTML = t("ram.body", "");
    body.appendChild(intro);

    const actions = document.createElement("div");
    actions.className = "bootmode-modal-actions";
    body.appendChild(actions);

    buildBootModeModalActions(actions);

    modal.appendChild(body);
    backdrop.appendChild(modal);

    backdrop.addEventListener("click", (event) => {
        if (event.target === backdrop) closeBootModeModal();
    });

    document.body.appendChild(backdrop);

    return backdrop;
}

/*
 * (Re)build the modal actions: one link per bootloader page of this build
 * plus the dismiss button.  Rebuilt whenever the page list changes, so a
 * FIP layout ends up offering the preloader next to the FIP.
 */
function buildBootModeModalActions(container) {
    while (container.firstChild) container.removeChild(container.firstChild);

    for (const action of bootModeActionNodes("button button-warn"))
        container.appendChild(action);

    const dismiss = bootModeTextNode("button", "button", "ram.dismiss", "");

    dismiss.type = "button";
    dismiss.addEventListener("click", closeBootModeModal);
    container.appendChild(dismiss);
}

function handleBootModeModalKey(event) {
    if (event.key === "Escape") closeBootModeModal();
}

function bootModeModalShown() {
    try {
        return sessionStorage.getItem(BOOTMODE_MODAL_KEY) === "1";
    } catch {
        return false;
    }
}

function openBootModeModal() {
    const backdrop = ensureBootModeModal();

    applyI18n(backdrop);
    backdrop.classList.add("is-open");
    document.addEventListener("keydown", handleBootModeModalKey);

    try {
        sessionStorage.setItem(BOOTMODE_MODAL_KEY, "1");
    } catch { /* ignore */ }
}

function closeBootModeModal() {
    const backdrop = document.getElementById("bootmode_modal_backdrop");

    if (backdrop) backdrop.classList.remove("is-open");
    document.removeEventListener("keydown", handleBootModeModalKey);
}

/*
 * Entry point: called whenever a boot mode becomes known (the /sysinfo
 * answer, which each page load fetches) and after a language switch, so both
 * the banner and the modal follow the report and stay translated.
 */
function applyBootModeWarning() {
    if (!bootModeIsRam()) {
        removeBootModeBanner();
        closeBootModeModal();
        return;
    }

    ensureBootModeBanner();

    /* Keep a modal that already exists in step with the page list, also
     * when it is not (or no longer) opened by this call. */
    const backdrop = document.getElementById("bootmode_modal_backdrop");
    const actions = backdrop?.querySelector(".bootmode-modal-actions");

    if (actions) buildBootModeModalActions(actions);

    /* The prominent modal belongs to the entry page of the UI. */
    if (APP_STATE.page !== "index" || bootModeModalShown()) return;

    openBootModeModal();
}

/*
 * Heading of the confirmation on the reboot page: reboot is the action that
 * turns a RAM session into a brick, so it is called out there too.
 */
function rebootWarningPrefix() {
    return bootModeIsRam() ? t("ram.reboot_confirm", "") : "";
}

const SIDEBAR_SCROLL_KEY = "failsafe_sidebar_scroll";

function readSidebarScroll() {
    try {
        const raw = sessionStorage.getItem(SIDEBAR_SCROLL_KEY);
        const n = raw === null ? NaN : parseInt(raw, 10);
        return Number.isFinite(n) && n >= 0 ? n : 0;
    } catch { return 0; }
}

function writeSidebarScroll(value) {
    const v = Math.max(0, value | 0);
    try { sessionStorage.setItem(SIDEBAR_SCROLL_KEY, String(v)); }
    catch { /* quota or disabled — ignore */ }
}

function attachSidebarScrollPersistence(navContainer) {
    const targetTop = readSidebarScroll();

    // The nav is the actual scroll container, but on a fresh page its layout
    // may not be ready immediately. Setting scrollTop before scrollHeight is
    // populated silently clamps to 0 — so retry across frames until either the
    // container becomes scrollable, or we give up.
    let attempts = 0;
    const tryRestore = () => {
        if (targetTop <= 0) return;
        const maxTop = navContainer.scrollHeight - navContainer.clientHeight;
        if (maxTop > 0) {
            navContainer.scrollTop = Math.min(targetTop, maxTop);
            return;
        }
        if (attempts++ < 30) requestAnimationFrame(tryRestore);
    };
    tryRestore();

    // Save scroll position, throttled via rAF.
    let rafId = 0;
    navContainer.addEventListener("scroll", () => {
        if (rafId) return;
        rafId = requestAnimationFrame(() => {
            rafId = 0;
            writeSidebarScroll(navContainer.scrollTop);
        });
    }, { passive: true });

    // Flush the current scroll position synchronously whenever the user is
    // about to leave the page — the throttled scroll write may not have fired
    // yet by the time navigation starts.
    const flush = () => writeSidebarScroll(navContainer.scrollTop);

    // Capture-phase click on links inside the sidebar: runs before the browser
    // begins navigation, while sessionStorage writes are still guaranteed.
    navContainer.addEventListener("click", (event) => {
        if (event.target.closest?.("a")) flush();
    }, true);

    window.addEventListener("pagehide", flush);
    // Some embedded browsers fire only beforeunload; cover both.
    window.addEventListener("beforeunload", flush);
}

function ajax(request) {
    const xhr = new XMLHttpRequest();
    xhr.upload.addEventListener("progress", (event) => request.progress?.(event));
    xhr.addEventListener("readystatechange", () => {
        if (xhr.readyState !== 4) return;
        if (xhr.status === 200) {
            request.done?.(xhr.responseText);
        } else {
            /* Hand the server's rejection to the caller: an ignored non-200
             * otherwise looks like a button that simply does nothing. */
            request.fail?.(xhr);
        }
    });
    if (request.timeout) xhr.timeout = request.timeout;
    const method = request.data ? "POST" : "GET";
    xhr.open(method, request.url);
    xhr.send(request.data);
}

/* consoleInit moved to console_js.js */

/* envInit moved to env_js.js */

function appInit(pageName) {
    APP_STATE.page = pageName || "";
    APP_STATE.i18nEnabled = isI18nAvailable();
    APP_STATE.lang = detectLang();
    APP_STATE.theme = detectTheme();
    setTheme(APP_STATE.theme, { persistEnv: false, persistLocal: true, silent: true });
    setLang(APP_STATE.lang);
    ensureSidebar();
    ensureBranding();
    ensureFavicon();
    applyI18n(document);
    updateDocumentTitle();
    loadThemeColor();
    loadThemeMode();
    loadDarkVariant();
    setTimeout(function () {
        document.body.classList.add("ready")
    }, 0);
    getversion();
    // Fetch system info for display
    getSysInfo();
    getSysInfoNand();
    pageName === "console" && typeof consoleInit === "function" && consoleInit();
    pageName === "env" && typeof envInit === "function" && envInit()
    pageName === "settings" && typeof settingsInit === "function" && settingsInit();
    pageName === "ubi" && typeof ubiInit === "function" && ubiInit();
    pageName === "fail" && typeof failInit === "function" && failInit();
    pageName === "flash" && typeof flashInit === "function" && flashInit();
    pageName === "simg" && typeof simgInit === "function" && simgInit();

    console.log('\n%c Yuzhii0718 ' + UBOOT_VERSION + ' %c ' + GITHUB_USER_URL + ' ', 'color: #fadfa3; background: #030307; padding:5px 0;', 'background: #fadfa3; padding:5px 0;');
}

/**
 * Unified nav visibility configuration.
 * Each entry defines how to determine if a nav link should be shown.
 * Pages controlled by Kconfig options are probed at runtime.
 * 
 * Mode: "probe" - fetch the page URL to check if it exists (default)
 *       "condition" - evaluate a condition function
 */
/*
 * Optional per-page metadata of the sidebar entries: the prefix used when
 * logging, the reason reported when the page is missing and extra work to
 * do in that case.
 *
 * Which entries are actually shown is decided by the firmware - the page
 * inventory reports itself through GET /ui/pages (see
 * failsafe/pages.c) - so a page that is missing here is still shown or
 * hidden correctly, only without the extra detail.
 */
const NAV_VISIBILITY_DEFS = {
    // Bootloader pages (selected by the failsafe layout / build mode)
    uboot: {
        url: "/uboot.html",
        logPrefix: "U-Boot",
        hiddenReason: "U-Boot layout only",
    },
    bl2: {
        url: "/bl2.html",
        logPrefix: "BL2",
        hiddenReason: "FIP layout only",
    },
    chainloader: {
        url: "/chainloader.html",
        logPrefix: "Chainloader",
        hiddenReason: "Chainloader layout only",
    },
    fip: {
        url: "/fip.html",
        logPrefix: "FIP",
        hiddenReason: "FIP layout only",
    },
    // Advanced features (depends on WEBUI_FAILSAFE_ADVANCED)
    console: {
        url: "/console.html",
        logPrefix: "Console",
        hiddenReason: "feature not enabled in build config",
    },
    env: {
        url: "/env.html",
        logPrefix: "Environment",
        hiddenReason: "feature not enabled in build config",
    },
    settings: {
        url: "/settings.html",
        logPrefix: "Settings",
        hiddenReason: "feature not enabled in build config",
        onHidden: ensureSidebarAccentFallback,
    },
    ubi: {
        url: "/ubi.html",
        logPrefix: "UBI",
        hiddenReason: "feature not enabled or no NAND flash",
    },
    flash: {
        url: "/flash.html",
        logPrefix: "Flash",
        hiddenReason: "feature not enabled in build config",
    },
    simg: {
        url: "/simg.html",
        logPrefix: "SIMG",
        hiddenReason: "feature not enabled in build config",
    },
    gpt: {
        url: "/gpt.html",
        logPrefix: "GPT",
        hiddenReason: "feature not enabled or no MMC device",
    },
};

/*
 * Show or hide one sidebar entry, and report why it stays hidden.
 *
 * NAV_VISIBILITY_DEFS only carries the extra detail here (log prefix, the
 * reason printed to the console, extra work to do when the entry does not
 * exist); whether a page was built into this firmware is decided by the
 * firmware itself - see applyNavVisibility().
 */
function setNavVisible(navId, visible) {
    const def = NAV_VISIBILITY_DEFS[navId] || {};
    const navLink = document.querySelector(`#sidebar [data-nav-id='${navId}']`);
    if (!navLink) return;

    navLink.style.display = visible ? "" : "none";

    if (!visible) {
        const reason = def.hiddenReason ? ` (${def.hiddenReason})` : "";
        console.warn(`${def.logPrefix || navId} not available${reason}`);
        if (def.onHidden) def.onHidden();
    }
}

/**
 * Unified nav visibility update function.
 * Supports two modes:
 *   - "probe": fetch URL to check if page exists
 *   - "condition": evaluate a condition function
 *
 * Falls back to probing the page URL, which also covers firmware that does
 * not answer GET /ui/pages (see applyNavVisibility()).
 *
 * @param {string} navId - The data-nav-id value
 */
function updateNavVisibility(navId) {
    const def = NAV_VISIBILITY_DEFS[navId] || {};
    const navLink = document.querySelector(`#sidebar [data-nav-id='${navId}']`);
    if (!navLink) return;

    if (def.mode === "condition") {
        // Condition-based visibility
        const show = def.condition();
        navLink.style.display = show ? "" : "none";
        console.warn(`${def.logPrefix || navId} nav visibility: ${show ? "shown" : "hidden"}`);
        return;
    }

    // Probe-based visibility (default)
    const probeKey = `_${navId}_probe_done`;
    const resultKey = `_${navId}_probe_result`;
    const sessionKey = `nav_probe_${navId}`;

    // Check sessionStorage first (persists across page navigations)
    try {
        const cached = sessionStorage.getItem(sessionKey);
        if (cached !== null) {
            const exists = cached === "1";
            APP_STATE[probeKey] = true;
            APP_STATE[resultKey] = exists;
            navLink.style.display = exists ? "" : "none";
            if (!exists && def.onHidden) def.onHidden();
            return;
        }
    } catch { /* sessionStorage unavailable */ }

    // If probe already done in this page session, use in-memory cached result
    if (APP_STATE[probeKey]) {
        navLink.style.display = APP_STATE[resultKey] ? "" : "none";
        return;
    }

    // No cached result: hide and start probe
    navLink.style.display = "none";
    APP_STATE[probeKey] = true;

    const url = def.url || navLink.getAttribute("href") || `/${navId}.html`;
    fetch(`${url}?_probe=1`, { method: "GET", cache: "no-store" })
        .then((response) => {
            const exists = response?.ok === true;
            APP_STATE[resultKey] = exists;
            try { sessionStorage.setItem(sessionKey, exists ? "1" : "0"); } catch {}
            setNavVisible(navId, exists);
        })
        .catch(() => {
            APP_STATE[resultKey] = false;
            try { sessionStorage.setItem(sessionKey, "0"); } catch {}
            setNavVisible(navId, false);
        });
}

/* ------------------------------------------------------------------ */
/*  Sidebar entries of this build                                      */
/* ------------------------------------------------------------------ */

/*
 * The entries that depend on the build (bl2, fip, gpt, ubi, ...) are created
 * hidden and revealed from the page list below, and that reveal must not
 * happen after the browser painted: doing it from an asynchronous answer is
 * what made the sidebar flicker - the entries appeared one by one after a
 * round trip, on every navigation.
 *
 * The list therefore has to be at hand while the sidebar is built, so it is
 * remembered for the session (sessionStorage survives the navigations of a
 * tab): ensureSidebar() applies it synchronously, and applyNavVisibility()
 * only refreshes a list that changed (a firmware update, or the first visit
 * in this tab).
 */
const PAGES_STORAGE_KEY = "failsafe_ui_pages";

/* Page ids GET /ui/pages reported for this firmware, or null when they are
 * not known yet. */
function readKnownPages() {
    try {
        const raw = sessionStorage.getItem(PAGES_STORAGE_KEY);
        if (!raw) return null;

        const list = JSON.parse(raw);

        return Array.isArray(list) ? list : null;
    } catch {
        return null;
    }
}

function rememberPages(pages) {
    try {
        sessionStorage.setItem(PAGES_STORAGE_KEY, JSON.stringify(pages));
    } catch { /* no sessionStorage: the list is fetched once per load */ }
}

/* Same ids in the same order? */
function samePages(a, b) {
    return Array.isArray(a) && Array.isArray(b) && a.length === b.length &&
        a.every((id, index) => id === b[index]);
}

/* Show or hide every sidebar entry according to @pages, the page ids of this
 * firmware. */
function applyPageList(pages) {
    for (const navLink of document.querySelectorAll("#sidebar [data-nav-id]")) {
        const navId = navLink.getAttribute("data-nav-id");

        if (navId) setNavVisible(navId, pages.includes(navId));
    }

    /* The RAM warning points at the bootloader pages of this build, which
     * are only known once this list is (see bootModeBootloaderPages). */
    applyBootModeWarning();
}

/**
 * Refresh the sidebar against the page list of the firmware.
 *
 * GET /ui/pages (see failsafe/pages.c) lists the pages of the build, so the
 * sidebar follows the firmware without a second list to keep in sync here.
 * The list of a build that is already known was applied by ensureSidebar();
 * this fetches it and re-applies it only when it differs, which is what
 * keeps the entries from appearing one by one while navigating.
 *
 * Firmware without that endpoint is handled by probing the optional
 * entries, which is what this UI used to do for every page.
 *
 * @param {?string[]} knownPages - page list already applied, if any
 */
async function applyNavVisibility(knownPages) {
    let pages = null;

    try {
        const response = await fetch("/ui/pages", { cache: "no-store" });
        if (response.ok) {
            const data = await response.json();
            if (Array.isArray(data.pages)) pages = data.pages;
        }
    } catch { /* fall back to probing */ }

    if (pages) {
        if (!samePages(pages, knownPages)) {
            rememberPages(pages);
            applyPageList(pages);
        }

        return;
    }

    if (knownPages) return;

    /* Unknown build: probe the entries that are hidden (the optional ones) */
    for (const navLink of document.querySelectorAll("#sidebar [data-nav-id]")) {
        const navId = navLink.getAttribute("data-nav-id");

        if (navId && navLink.style.display === "none")
            updateNavVisibility(navId);
    }
}

function ensureSidebarAccentFallback() {
    const controlsContainer = document.querySelector("#sidebar .sidebar-controls");
    if (!controlsContainer || controlsContainer.querySelector(".control-row-color")) return;
    appendAccentControls(controlsContainer);
    applyI18n(controlsContainer);
}

/*
 * ATF (BL2 / BL31) version row of the "More info" block.
 *
 * ARM only: ATF is Trusted Firmware-A, so /sysinfo reports the capability
 * and non-ARM devices never call this - the row is simply not there.
 *
 * Reading the banners back from flash costs a storage read (and, for
 * BL31, a decompression) on the device, so nothing is fetched until the
 * user presses the button.  The answer is kept in APP_STATE so that
 * re-rendering the block (language switch, sysinfo refresh) does not
 * lose it.
 */
function renderSysInfoAtf(extra) {
    const row = document.createElement("div");
    row.className = "sysinfo-atf-head";

    const section = document.createElement("div");
    section.className = "sysinfo-section";
    section.setAttribute("data-sysinfo", "atf");
    section.textContent = t("sysinfo.atf", "ATF version:");
    row.appendChild(section);

    const button = document.createElement("button");
    /* type=button: the sysinfo block lives inside the upload <form>. */
    button.type = "button";
    button.className = "button button-sm";
    button.textContent = t("sysinfo.atf.fetch", "Fetch");
    row.appendChild(button);

    const output = document.createElement("div");
    output.className = "sysinfo-atf-output";
    output.hidden = true;
    if (APP_STATE.atfText) {
        output.textContent = APP_STATE.atfText;
        output.hidden = false;
    }

    const fetchAtf = async () => {
        button.disabled = true;
        output.hidden = false;
        output.textContent = t("sysinfo.atf.loading", "Reading...");
        try {
            const response = await fetch("/atfversion", { cache: "no-store" });
            if (!response.ok) throw new Error(String(response.status));
            APP_STATE.atfText = (await response.text()).trim();
            output.textContent = APP_STATE.atfText;
        } catch {
            output.textContent = t("sysinfo.atf.error", "Failed to read the ATF version");
        } finally {
            button.disabled = false;
        }
    };

    button.addEventListener("click", fetchAtf);

    extra.appendChild(row);
    extra.appendChild(output);
}

function renderSysInfo() {
    const sysinfoContainer = document.getElementById("sysinfo");
    let sysinfoData;
    let boardInfo;
    let ramInfo;
    if (!sysinfoContainer) return;
    sysinfoData = APP_STATE.sysinfo;
    if (!sysinfoData) {
        sysinfoContainer.textContent = t("sysinfo.loading");
        return
    }
    boardInfo = sysinfoData.board || {};
    ramInfo = sysinfoData.ram || {};

    while (sysinfoContainer.firstChild) sysinfoContainer.removeChild(sysinfoContainer.firstChild);
    sysinfoContainer.classList.remove("sysinfo-expanded");

    const summary = document.createElement("div");
    summary.className = "sysinfo-summary";

    const boardLine = document.createElement("div");
    boardLine.className = "sysinfo-line";
    boardLine.textContent = t("sysinfo.board") + " " + (boardInfo.model || t("sysinfo.unknown"));
    summary.appendChild(boardLine);

    if (sysinfoData.soc) {
        const socLine = document.createElement("div");
        socLine.className = "sysinfo-line";
        socLine.textContent = t("sysinfo.soc") + " " + sysinfoData.soc;
        summary.appendChild(socLine);
    }

    const ramLine = document.createElement("div");
    ramLine.className = "sysinfo-line";
    ramLine.textContent = t("sysinfo.ram") + " " + (ramInfo.size !== undefined && ramInfo.size !== null && ramInfo.size !== 0 ? bytesToHuman(ramInfo.size) : t("sysinfo.unknown"));
    summary.appendChild(ramLine);

    sysinfoContainer.appendChild(summary);

    const details = document.createElement("details");
    details.className = "sysinfo-details";

    const summaryNode = document.createElement("summary");
    summaryNode.textContent = t("sysinfo.more", "More info");
    details.appendChild(summaryNode);

    const extra = document.createElement("div");
    extra.className = "sysinfo-extra";

    if (sysinfoData.build_variant) {
        const variantLine = document.createElement("div");
        variantLine.className = "sysinfo-line";
        variantLine.textContent = t("sysinfo.variant", "Variant") + " " + sysinfoData.build_variant;
        extra.appendChild(variantLine);
    }

    if (boardInfo.compatible) {
        const compatLine = document.createElement("div");
        compatLine.className = "sysinfo-line";
        compatLine.textContent = t("sysinfo.compat", "Compatible") + " " + boardInfo.compatible;
        extra.appendChild(compatLine);
    }

    /* On-demand ATF (BL2 / BL31) row - never fetched automatically, and
     * only offered on ARM, where the device reports the capability. */
    if (sysinfoData.atf) renderSysInfoAtf(extra);

    if (extra.childNodes.length) {
        details.appendChild(extra);
        sysinfoContainer.appendChild(details);

        const toggleExpanded = () => {
            details.open ? sysinfoContainer.classList.add("sysinfo-expanded") : sysinfoContainer.classList.remove("sysinfo-expanded");
        };
        details.addEventListener("toggle", toggleExpanded);
        toggleExpanded();
    }

    renderSysInfoNand();
}

function renderSysInfoNand() {
    const sysinfoContainer = document.getElementById("sysinfo");
    if (!sysinfoContainer) return;

    const nandData = APP_STATE.sysinfoNand;
    if (!nandData) return;

    const nand = nandData.nand;
    const mmc = nandData.mmc;

    /* Flash model: prefer the NAND chip; fall back to the MMC device on
     * boards that boot from eMMC / SD (no NAND).  Without the fallback the
     * home page printed "闪存：未知" even though /flash/info had the data. */
    let model = null;
    if (nand && nand.present && nand.model)
        model = nand.model;
    else if (mmc && mmc.present) {
        const v = (mmc.vendor || "").trim();
        const p = (mmc.product || "").trim();
        model = (v + " " + p).trim();
    }

    // Flash model: append to the summary block (after RAM line)
    const summary = sysinfoContainer.querySelector(".sysinfo-summary");
    if (model && summary) {
        let flashLine = summary.querySelector("[data-sysinfo='flash']");
        if (!flashLine) {
            flashLine = document.createElement("div");
            flashLine.className = "sysinfo-line";
            flashLine.setAttribute("data-sysinfo", "flash");
            summary.appendChild(flashLine);
        }
        flashLine.textContent = t("sysinfo.flash") + " " + model;
    }

    // Partitions: merge into the "More info" details/extra block.
    // Prefer NAND parts, otherwise the MMC ones.
    const parts = (nand && Array.isArray(nand.parts) && nand.parts.length) ? nand.parts
                : (mmc && Array.isArray(mmc.parts) ? mmc.parts : []);

    if (parts.length) {
        let details = sysinfoContainer.querySelector(".sysinfo-details");
        let extra = sysinfoContainer.querySelector(".sysinfo-extra");

        // Create the details/extra block if it was not built yet (no variant/compatible)
        if (!details) {
            details = document.createElement("details");
            details.className = "sysinfo-details";

            const summaryNode = document.createElement("summary");
            summaryNode.textContent = t("sysinfo.more", "More info");
            details.appendChild(summaryNode);
            sysinfoContainer.appendChild(details);

            const toggleExpanded = () => {
                details.open ? sysinfoContainer.classList.add("sysinfo-expanded") : sysinfoContainer.classList.remove("sysinfo-expanded");
            };
            details.addEventListener("toggle", toggleExpanded);
            toggleExpanded();
        }
        if (!extra) {
            extra = document.createElement("div");
            extra.className = "sysinfo-extra";
            details.appendChild(extra);
        }

        // Refresh any stale partition block before re-adding
        const prevPart = extra.querySelector("[data-sysinfo='partitions']");
        if (prevPart) prevPart.remove();

        const section = document.createElement("div");
        section.className = "sysinfo-section";
        section.setAttribute("data-sysinfo", "partitions");
        section.textContent = t("sysinfo.partitions");
        extra.appendChild(section);

        const list = document.createElement("div");
        list.className = "sysinfo-list";
        parts.forEach((part) => {
            const item = document.createElement("div");
            item.textContent = part.name + (part.master ? " (" + t("sysinfo.master") + ")" : "") + " - " + bytesToHuman(part.size);
            list.appendChild(item);
        });
        extra.appendChild(list);
    }
}

function getSysInfo() {
    // Always fetch sysinfo into APP_STATE, but only render when the sysinfo
    // element exists on current page.
    const sysinfoElement = document.getElementById("sysinfo");
    if (sysinfoElement) renderSysInfo();
    ajax({
        url: "/sysinfo",
        done: (responseText) => {
            try {
                APP_STATE.sysinfo = JSON.parse(responseText);
            } catch {
                return;
            }
            updateFipMaxSizeLabels();
            if (sysinfoElement) renderSysInfo();
            /* A RAM (console recovery) session is warned about on every
             * page; see applyBootModeWarning(). */
            applyBootModeWarning();
        },
    });
}

function getSysInfoNand() {
    ajax({
        url: "/sysinfo/nand",
        done: (responseText) => {
            try {
                APP_STATE.sysinfoNand = JSON.parse(responseText);
            } catch {
                return;
            }
            if (document.getElementById("sysinfo")) renderSysInfoNand();
        },
    });
}

async function ensureSysInfoLoaded() {
    // On pages without #sysinfo, we still need board model.
    if (APP_STATE.sysinfo?.board?.model) return APP_STATE.sysinfo;
    if (APP_STATE._sysinfo_promise) return APP_STATE._sysinfo_promise;

    APP_STATE._sysinfo_promise = (async () => {
        try {
            const response = await fetch("/sysinfo", { method: "GET" });
            if (!response?.ok) return null;
            const payload = await response.json();
            if (payload) APP_STATE.sysinfo = payload;
            return payload;
        } catch {
            return null;
        } finally {
            // allow retry later
            APP_STATE._sysinfo_promise = null;
        }
    })();

    return APP_STATE._sysinfo_promise;
}

function startup() {
    appInit("index")
}

function getversion() {
    ajax({
        url: "/version",
        done: (versionText) => {
            const versionElement = document.getElementById("version");
            if (versionElement) versionElement.innerHTML = versionText;
            ensureBranding();
        },
    });
}

/**
 * Show what the firmware reported about the failed upgrade on fail.html.
 *
 * GET /last-error (see failsafe/modules/upgrade.c, filled by the failure
 * sites through <failsafe/error.h>) carries the code and the message of the
 * failure, so the reason is visible on the page instead of only on the
 * U-Boot console.  Without a report the page keeps its generic text.
 */
async function failInit() {
    const box = document.getElementById("error_box");
    if (!box) return;

    let report = null;
    try {
        const response = await fetch("/last-error", { cache: "no-store" });
        if (response.ok) report = await response.json();
    } catch { /* keep the generic text */ }

    const code = report?.code ?? 0;
    const message = String(report?.error ?? "").trim();
    if (!message && !code) return;

    const textElement = document.getElementById("error_text");
    const codeElement = document.getElementById("error_code");
    if (textElement) {
        textElement.textContent = message ||
            t("fail.msg.unknown", "no details were reported");
    }
    if (codeElement) codeElement.textContent = String(code);
    box.style.display = "";
}

/*
 * Size of the storage holding the FIP, as reported by GET /sysinfo
 * (CONFIG_WEBUI_FAILSAFE_FIP_SIZE: 1 MiB on Airoha / EcoNet, 2 MiB on
 * MediaTek).  Returns 0 while the answer has not arrived yet or on a build
 * that does not report it; the client side check is then skipped and the
 * firmware's own capacity check remains the only limit.
 */
function fipMaxSize() {
    const size = Number(APP_STATE.sysinfo?.fip_size);
    return Number.isFinite(size) && size > 0 ? size : 0;
}

/*
 * Put the configured FIP size into the [data-fip-max-size] placeholders of
 * the FIP page.  applyI18n() re-renders the hint on every language switch,
 * which is why this runs after it (see setLang()) and again once /sysinfo
 * has answered (see getSysInfo()).
 */
function updateFipMaxSizeLabels() {
    const limit = fipMaxSize();
    if (!limit) return;

    for (const node of document.querySelectorAll("[data-fip-max-size]"))
        node.textContent = bytesToHuman(limit);
}

function upload(formFieldName) {
    const selectedFile = document.getElementById("file").files[0];
    if (!selectedFile) return;

    /* The FIP storage is created at the size the build configures, so
     * reject oversized images before uploading them.  The firmware applies
     * the same limit again and reports the reason on the fail page. */
    const fipLimit = formFieldName === "fip" ? fipMaxSize() : 0;
    if (fipLimit && selectedFile.size > fipLimit) {
        alert(t("fip.err.too_big"));
        return;
    }

    /* The GPT image is the primary table area only (34 sectors =
     * 17408 bytes, see FAILSAFE_STORAGE_GPT_MAX_SIZE); the backend
     * rejects anything larger, so catch it before the upload. */
    if (formFieldName === "gpt" && selectedFile.size > 17408) {
        alert(t("gpt.err.too_big"));
        return;
    }

    const selectedFileName = selectedFile.name || "";

    const formElement = document.getElementById("form");
    if (formElement) formElement.style.display = "none";

    const hintElement = document.getElementById("hint");
    if (hintElement) hintElement.style.display = "none";

    const progressBarElement = document.getElementById("bar");
    if (progressBarElement) progressBarElement.style.display = "block";

    const formData = new FormData();
    formData.append(formFieldName, selectedFile);

    ajax({
        url: "/upload",
        data: formData,
        done: (responseText) => {
            /* The status word is the first line; on failure the firmware
             * appends "code:" / "error:" lines saying why it rejected the
             * image (also available from /last-error, which the fail page
             * reads after the redirect). */
            if (String(responseText).split("\n")[0].trim() === "fail") {
                location = "/fail.html";
                return;
            }
            /*
             * First line: "<size> <md5>" (the historical format).
             * Optional following lines carry extra upload metadata as
             * "key:value" pairs, currently the BL2 (preloader) banner:
             *     bl2_version:v2.10.0 (release):00dba2b
             *     bl2_date:14:09:01, Sep 11 2026
             */
            const responseLines = String(responseText).split("\n");
            const [sizeText, md5Text] = responseLines[0].split(" ");
            const uploadMeta = {};
            for (let lineIndex = 1; lineIndex < responseLines.length; lineIndex++) {
                const separator = responseLines[lineIndex].indexOf(":");
                if (separator > 0) {
                    uploadMeta[responseLines[lineIndex].slice(0, separator)] =
                        responseLines[lineIndex].slice(separator + 1).trim();
                }
            }

            const filenameElement = document.getElementById("filename");
            if (filenameElement && selectedFileName) {
                filenameElement.style.display = "block";
                filenameElement.innerHTML =
                    `<span class="filename-label">${t("label.file")}</span>` +
                    `<span class="filename-value">${selectedFileName}</span>`;
            }

            const sizeElement = document.getElementById("size");
            if (sizeElement) {
                sizeElement.style.display = "block";
                const parsedSize = parseInt(sizeText, 10);
                sizeElement.innerHTML = `${t("label.size")}${isFinite(parsedSize) ? bytesToHuman(parsedSize) : sizeText}`;
            }

            const md5Element = document.getElementById("md5");
            if (md5Element) {
                const md5Match = selectedFileName
                    ? /(?:^|[._-])md5-([0-9a-fA-F]{32})(?:$|[._-])/.exec(selectedFileName)
                    : null;
                const md5InName = md5Match?.[1] ?? "";
                const md5Ok = !!(md5Text && md5InName &&
                    md5Text.toLowerCase() === md5InName.toLowerCase());
                const md5Hint  = md5InName ? (md5Ok ? t("md5.match") : t("md5.mismatch")) : "";
                const md5Class = md5InName ? (md5Ok ? "md5-ok" : "md5-bad") : "";
                md5Element.style.display = "block";
                md5Element.innerHTML = `${t("label.md5")}${md5Text}` + (
                    md5Hint ? ` <span class="md5-status ${md5Class}">${md5Hint}</span>` : ""
                );
            }

            const bl2VersionElement = document.getElementById("bl2_version");
            if (bl2VersionElement && uploadMeta.bl2_version) {
                bl2VersionElement.style.display = "block";
                bl2VersionElement.textContent =
                    t("label.bl2_version") + uploadMeta.bl2_version;
            }

            const bl2DateElement = document.getElementById("bl2_date");
            if (bl2DateElement && uploadMeta.bl2_date) {
                bl2DateElement.style.display = "block";
                bl2DateElement.textContent =
                    t("label.bl2_date") + uploadMeta.bl2_date;
            }

            const upgradeElement = document.getElementById("upgrade");
            if (upgradeElement) upgradeElement.style.display = "block";

        },
        progress: (progressEvent) => {
            if (!progressEvent.total) return;
            const percent = Math.floor(progressEvent.loaded / progressEvent.total * 100);
            const progressElement = document.getElementById("bar");
            if (progressElement) {
                progressElement.style.display = "block";
                progressElement.style.setProperty("--percent", percent);
            }
            const uploadHero = document.getElementById("upload_hero");
            if (uploadHero) uploadHero.style.display = "";
            const barText = document.getElementById("bar_text");
            if (barText) {
                barText.style.display = "block";
                barText.textContent = percent + "%";
            }
        },
    });
}

const BYTE_UNITS = [
    { threshold: 1024 ** 3, suffix: " GiB" },
    { threshold: 1024 ** 2, suffix: " MiB" },
    { threshold: 1024,      suffix: " KiB" },
];

function bytesToHuman(bytes) {
    if (bytes == null) return "";
    const n = Number(bytes);
    if (!Number.isFinite(n) || n < 0) return "";
    for (const { threshold, suffix } of BYTE_UNITS) {
        if (n >= threshold) return (n / threshold).toFixed(2) + suffix;
    }
    return `${Math.floor(n)} B`;
}

function parseFilenameFromDisposition(dispositionHeader) {
    if (!dispositionHeader) return "";
    const quoted = /filename\s*=\s*"([^"]+)"/i.exec(dispositionHeader);
    if (quoted?.[1]) return quoted[1];
    const unquoted = /filename\s*=\s*([^;\s]+)/i.exec(dispositionHeader);
    return unquoted?.[1]?.replace(/^"|"$/g, "") ?? "";
}

function sanitizeFilenameComponent(value) {
    return value
        ? String(value).replace(/[^a-zA-Z0-9._-]+/g, "_").replace(/^_+|_+$/g, "").slice(0, 48)
        : "";
}

function getNowYYYYMMDD() {
    const now = new Date();
    const year  = now.getFullYear();
    const month = String(now.getMonth() + 1).padStart(2, "0");
    const day   = String(now.getDate()).padStart(2, "0");
    return `${year}${month}${day}`;
}

function makeBackupDownloadName(originalName) {
    const boardModel = APP_STATE.sysinfo?.board?.model ?? "";
    const boardComponent = sanitizeFilenameComponent(boardModel) || "board";
    const dateStamp = getNowYYYYMMDD();
    let downloadName = String(originalName || "backup.bin");

    // Ensure it starts with backup_
    if (!downloadName.startsWith("backup_")) {
        downloadName = "backup_" + downloadName.replace(/^_+/, "");
    }

    // Insert board right after backup_ if not already
    if (!downloadName.startsWith(`backup_${boardComponent}_`)) {
        downloadName = downloadName.replace(/^backup_/, `backup_${boardComponent}_`);
    }

    // Ensure .bin extension
    if (!/\.[A-Za-z0-9]+$/.test(downloadName)) {
        downloadName += ".bin";
    }

    // Append date before extension if not already present
    if (!/_\d{8}\.[A-Za-z0-9]+$/.test(downloadName)) {
        downloadName = downloadName.replace(/(\.[A-Za-z0-9]+)$/, `_${dateStamp}$1`);
    }

    return downloadName;
}

const SIZE_SUFFIX_MULTIPLIERS = {
    "":    1,
    k:     1024,        kb:  1024,        kib: 1024,
    m:     1024 ** 2,   mb:  1024 ** 2,   mib: 1024 ** 2,
    g:     1024 ** 3,   gb:  1024 ** 3,   gib: 1024 ** 3,
};

function parseUserLen(input) {
    if (!input) return null;
    const trimmed = String(input).trim();
    if (!trimmed) return null;
    const match = /^\s*(0x[0-9a-fA-F]+|\d+)\s*([a-zA-Z]*)\s*$/.exec(trimmed);
    if (!match) return null;

    const rawNumber = match[1];
    const suffix    = match[2].toLowerCase();
    const numericValue = rawNumber.toLowerCase().startsWith("0x")
        ? parseInt(rawNumber, 16)
        : parseInt(rawNumber, 10);
    if (!Number.isFinite(numericValue) || numericValue < 0) return null;

    const multiplier = SIZE_SUFFIX_MULTIPLIERS[suffix];
    return multiplier === undefined ? null : Math.floor(numericValue * multiplier);
}
