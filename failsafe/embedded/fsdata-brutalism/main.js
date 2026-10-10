/* ==========================================================================
   main.js - Airoha U-Boot Recovery (Brutalism UI)
   AJAX upload / inline confirm / inline error / language switch / result i18n
   ========================================================================== */

(function (w, d) {
	"use strict";

	/* ---- Element IDs to hide during upload confirmation ----
	 * Includes #section-uboot (its <p> title is NOT a direct child of
	 * #main, so it would otherwise stay visible during confirmation).
	 */
	var UPLOAD_HIDE_IDS = [
		"section-uboot",
		"form-firmware", "form-uboot", "form-initramfs", "form-reboot"
	];

	/* ---- Show #upload-result and hide upload forms ---- */
	function hideUploadForms() {
		for (var i = 0; i < UPLOAD_HIDE_IDS.length; i++) {
			var el = d.getElementById(UPLOAD_HIDE_IDS[i]);
			if (el) el.style.display = "none";
		}
		var titles = d.querySelectorAll("#main > p");
		for (var i = 0; i < titles.length; i++) titles[i].style.display = "none";
		var hrs = d.querySelectorAll("hr");
		for (var j = 0; j < hrs.length; j++) hrs[j].style.display = "none";
	}

	/* ---- Show only the warning block for the given type ---- */
	function showTypeWarnings(fieldName) {
		var ids = ["warn-firmware", "warn-uboot", "warn-initramfs"];
		for (var i = 0; i < ids.length; i++) {
			var el = d.getElementById(ids[i]);
			if (el) el.style.display = (ids[i] === "warn-" + fieldName) ? "block" : "none";
		}
	}

	/* ---- Show inline error, hide warnings + proceed ---- */
	function showUploadError() {
		var warn = d.querySelectorAll(".type-warnings");
		for (var i = 0; i < warn.length; i++) warn[i].style.display = "none";
		var proc = d.getElementById("proceed-section");
		if (proc) proc.style.display = "none";
		var err = d.getElementById("upload-error");
		if (err) err.style.display = "block";
	}

	/* ---- AJAX upload to /upload ----
	 * fieldName: firmware | uboot | initramfs
	 * Server responds: "<size> <md5>" on success (optionally followed by
	 * "key:value" lines, e.g. the BL2 banner for bl2/uboot uploads),
	 * "fail" on validation error
	 * Success: show MD5/size + type-specific warnings + Proceed button
	 * Failure: show inline error with retry button
	 */
	w.brutalismUpload = function (fieldName) {
		var f = d.getElementById("file-" + fieldName);
		if (!f || !f.files || !f.files[0]) return;

		var fd = new FormData();
		fd.append(fieldName, f.files[0]);

		hideUploadForms();

		/* Reset error/proceed visibility */
		var errEl = d.getElementById("upload-error");
		if (errEl) errEl.style.display = "none";
		var procEl = d.getElementById("proceed-section");
		if (procEl) procEl.style.display = "block";

		var x = new XMLHttpRequest();
		x.open("POST", "/upload");
		x.timeout = 300000;
		x.onreadystatechange = function () {
			if (x.readyState !== 4) return;

			var res = d.getElementById("upload-result");
			if (!res) return;

			/* Determine success or failure: the status word is the
			 * first line, a rejected upload may append "code:" /
			 * "error:" lines saying why (see GET /last-error). */
			var ok = (x.status === 200);
			var r = x.responseText || "";
			if (ok && r.split("\n")[0].trim() === "fail")
				ok = false;

			if (!ok) {
				/* Show inline error */
				var titleEl = d.getElementById("active-type-title");
				if (titleEl) {
					titleEl.setAttribute("data-i18n", "fail.title");
					titleEl.textContent = "\u2588\u2588\u2588 UPGRADE FAILED! SYSTEM ERROR \u2588\u2588\u2588";
				}
				var fi = d.getElementById("fileinfo");
				if (fi) fi.style.display = "none";
				showUploadError();
				res.style.display = "block";
				if (w.i18n) w.i18n.applyTranslations(res);
				w.brutalismShowReportedFailure(res);
				return;
			}

			/* Success: show MD5/size + type-specific warnings.
			 * The first response line is "<size> <md5>"; any extra
			 * "key:value" lines (e.g. the BL2 banner reported for
			 * bl2/uboot uploads) are ignored here. */
			var p = r.split("\n")[0].split(" ");
			var m = d.getElementById("md5-value");
			var sz = d.getElementById("size-value");
			if (m) m.textContent = p[1];
			if (sz) sz.textContent = p[0];

			var typeKeys = {
				"firmware": "index.fw_ubi",
				"uboot": "index.uboot",
				"initramfs": "index.initramfs"
			};
			var titleEl2 = d.getElementById("active-type-title");
			if (titleEl2) {
				titleEl2.setAttribute("data-i18n", typeKeys[fieldName] || "");
				var fallback = {
					"firmware": "\u25B6 UPGRADE SYSTEM FIRMWARE (UBI VOLUME)",
					"uboot": "\u25B6 UPGRADE U-BOOT (MTD PARTITION)",
					"initramfs": "\u25B6 UPLOAD BOOT IMAGE (RAM BOOT)"
				};
				titleEl2.textContent = fallback[fieldName] || fieldName;
			}

			showTypeWarnings(fieldName);
			res.style.display = "block";
			if (w.i18n) w.i18n.applyTranslations(res);
		};
		x.send(fd);
	};

	/* ---- Replace the decorative error code with the reported one ---- */
	/*
	 * GET /last-error (see failsafe/modules/upgrade.c, fed by the failure
	 * sites through <failsafe/error.h>) carries the code and the message
	 * of the failed upgrade, so the reason shows up in the page instead of
	 * only on the U-Boot console.  Without a report the static text stays.
	 */
	w.brutalismShowReportedFailure = function (root) {
		if (!root || !w.fetch) return;

		w.fetch("/last-error", { cache: "no-store" }).then(function (r) {
			return r.ok ? r.json() : null;
		}).then(function (report) {
			var el;

			if (!report || (!report.code && !report.error)) return;

			el = root.querySelector('[data-i18n="fail.code"] strong') ||
				root.querySelector('[data-i18n="fail.code"]');
			if (!el) return;

			el.textContent = (report.code ? String(report.code) : "?") +
				(report.error ? " - " + report.error : "");
		}).catch(function () { /* keep the static text */ });
	};

	/* ---- RAM (console recovery) session warning ----
	 * GET /sysinfo reports "boot.mode" (see common/boot_mode.c and
	 * failsafe/modules/sysinfo.c): "ram" means the bootloader was fetched
	 * over the console by the previous boot stage and runs from DRAM
	 * only, so nothing has reached the flash yet.  Uploading only a
	 * firmware image, or rebooting, then leaves a device whose bootloader
	 * is missing or does not match the installed firmware - the usual way
	 * a device bricks.
	 *
	 * Only "ram" warns; "unknown" (a boot chain that cannot report the
	 * mode) must not produce a false alarm.  The banner stays on the page,
	 * the modal is shown once per browser session.
	 */
	var bootMode = "unknown";
	var RAM_MODAL_KEY = "failsafe_ram_modal_shown";

	function bootModeIsRam() { return bootMode === "ram"; }

	function ramText(key) {
		return (w.i18n && w.i18n.t) ? w.i18n.t(key) : key;
	}

	function showRamBanner() {
		var main = d.getElementById("main");
		if (!main || d.getElementById("ram-banner")) return;

		var el = d.createElement("div");
		el.id = "ram-banner";
		el.className = "ram-banner";
		el.setAttribute("role", "alert");

		var tag = d.createElement("strong");
		tag.setAttribute("data-i18n", "ram.badge");
		tag.textContent = ramText("ram.badge");
		el.appendChild(tag);

		var text = d.createElement("span");
		text.setAttribute("data-i18n", "ram.banner");
		text.textContent = ramText("ram.banner");
		el.appendChild(text);

		var link = d.createElement("a");
		link.href = "#section-uboot";
		link.setAttribute("data-i18n", "ram.bootloader");
		link.textContent = ramText("ram.bootloader");
		el.appendChild(link);

		main.insertBefore(el, main.firstChild);
	}

	function closeRamModal() {
		var overlay = d.getElementById("ram-modal");
		if (overlay) overlay.parentNode.removeChild(overlay);
	}

	function showRamModal() {
		if (d.getElementById("ram-modal")) return;

		var overlay = d.createElement("div");
		overlay.id = "ram-modal";
		overlay.className = "ram-modal";

		var box = d.createElement("div");
		box.className = "ram-modal-box";
		box.setAttribute("role", "dialog");
		box.setAttribute("aria-modal", "true");

		var title = d.createElement("p");
		title.className = "ram-modal-title";
		title.setAttribute("data-i18n", "ram.title");
		title.textContent = ramText("ram.title");
		box.appendChild(title);

		var body = d.createElement("p");
		body.className = "ram-modal-body";
		body.setAttribute("data-i18n", "ram.body");
		body.innerHTML = ramText("ram.body");
		box.appendChild(body);

		var go = d.createElement("a");
		go.className = "ram-modal-btn";
		go.href = "#section-uboot";
		go.setAttribute("data-i18n", "ram.bootloader");
		go.textContent = ramText("ram.bootloader");
		go.addEventListener("click", closeRamModal);
		box.appendChild(go);

		var ok = d.createElement("button");
		ok.type = "button";
		ok.className = "ram-modal-btn";
		ok.setAttribute("data-i18n", "ram.dismiss");
		ok.textContent = ramText("ram.dismiss");
		ok.addEventListener("click", closeRamModal);
		box.appendChild(ok);

		overlay.appendChild(box);
		d.body.appendChild(overlay);
	}

	function applyBootMode() {
		if (!bootModeIsRam()) return;

		showRamBanner();

		try {
			if (sessionStorage.getItem(RAM_MODAL_KEY) === "1") return;
			sessionStorage.setItem(RAM_MODAL_KEY, "1");
		} catch (e) { /* no sessionStorage: show it once per load */ }

		showRamModal();
	}

	function fetchBootMode() {
		var x = new XMLHttpRequest();
		x.open("GET", "/sysinfo");
		x.timeout = 3000;
		x.onreadystatechange = function () {
			if (x.readyState !== 4 || x.status !== 200) return;

			try {
				var info = JSON.parse(x.responseText);
				bootMode = (info && info.boot && info.boot.mode) || "unknown";
			} catch (e) {
				return;
			}

			applyBootMode();
		};
		x.send();
	}

	/* ---- Reset: hide #upload-result, show all upload forms ---- */
	w.brutalismResetUpload = function () {
		var res = d.getElementById("upload-result");
		if (res) res.style.display = "none";

		/* Show all forms and titles */
		for (var i = 0; i < UPLOAD_HIDE_IDS.length; i++) {
			var el = d.getElementById(UPLOAD_HIDE_IDS[i]);
			if (el) el.style.display = "";
		}
		var titles = d.querySelectorAll("#main > p");
		for (var i = 0; i < titles.length; i++) titles[i].style.display = "";
		var hrs = d.querySelectorAll("hr");
		for (var j = 0; j < hrs.length; j++) hrs[j].style.display = "";

		/* Reset file inputs */
		var inputs = d.querySelectorAll("input[type=file]");
		for (var k = 0; k < inputs.length; k++) inputs[k].value = "";

		/* Reset error/proceed visibility for next upload */
		var errEl = d.getElementById("upload-error");
		if (errEl) errEl.style.display = "none";
		var procEl = d.getElementById("proceed-section");
		if (procEl) procEl.style.display = "block";
		var fi = d.getElementById("fileinfo");
		if (fi) fi.style.display = "";
	};

	/* ---- Reboot control: navigate to /reboot.html first, then trigger
	 * the actual action from that page once it has finished loading.
	 *   mode="failsafe": reboot back into failsafe mode (sets env then resets)
	 *   mode="boot":     run bootcmd and boot the installed firmware directly
	 *   mode="normal" (default, also for legacy falsy arg): normal reboot
	 * We MUST NOT send the GET /reboot (or /boot) request here because the
	 * device would reset/boot before /reboot.html (its visual feedback
	 * page) could be fetched, leaving the browser with no resources to render.
	 */
	w.brutalismReboot = function (mode) {
		var m = (mode === "failsafe") ? "failsafe" :
			(mode === "boot") ? "boot" : "normal";

		/* Rebooting is the action that turns a RAM session into a brick,
		 * so it is called out here as well as by the banner. */
		if (bootModeIsRam() && !w.confirm(ramText("ram.reboot_confirm")))
			return;

		w.location = "/reboot.html?mode=" + m;
	};

	/* ---- Language switcher ---- */
	function initLang() {
		var b = d.getElementById("lang-switch");
		if (!b) return;
		b.addEventListener("click", function () { if (w.i18n) w.i18n.toggleLang(); });
	}

	/* ---- Fetch /version and stamp it on #banner[data-version] ----
	 * Server returns plain text like:
	 *   "U-Boot 2024.10 abcd123-dirty wifi7"
	 * We show it in the banner's ::after pseudo-element via attr(data-version).
	 */
	function fetchVersion() {
		var banner = d.getElementById("banner");
		if (!banner) return;
		var x = new XMLHttpRequest();
		x.open("GET", "/version");
		x.timeout = 3000;
		x.onreadystatechange = function () {
			if (x.readyState !== 4) return;
			if (x.status !== 200 || !x.responseText) return;
			var v = x.responseText.trim();
			if (v) banner.setAttribute("data-version", "[" + v + "]");
		};
		x.send();
	}

	/* ---- Patch result AJAX to re-apply i18n (used by flashing.html) ---- */
	function patchResultLoader() {
		var m = d.getElementById("main");
		if (!m || !w.MutationObserver) return;
		var obs = new w.MutationObserver(function (muts) {
			var added = false;
			for (var i = 0; i < muts.length; i++) {
				if (muts[i].addedNodes.length > 0) { added = true; break; }
			}
			if (!added || !w.i18n) return;
			obs.disconnect();
			w.i18n.applyTranslations(m);
			obs.observe(m, { childList: true, subtree: true });
		});
		obs.observe(m, { childList: true, subtree: true });
	}

	function init() {
		initLang();
		fetchVersion();
		fetchBootMode();
		patchResultLoader();
	}

	if (d.readyState === "loading") d.addEventListener("DOMContentLoaded", init);
	else init();

})(window, document);
