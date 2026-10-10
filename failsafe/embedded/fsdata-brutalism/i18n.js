/* ==========================================================================
   i18n.js - Internationalization (Brutalism UI, U-Boot layout focused)
   Languages: English (en) / 中文 (zh)
   ========================================================================== */

(function (w) {
	"use strict";

	var T = {
		en: {
			/* Common */
			"banner.title": "AIROHA U-BOOT SYSTEM RECOVERY",
			/* RAM (console recovery) session warning - see main.js and
			 * common/boot_mode.c. */
			"ram.badge": "RAM",
			"ram.banner": "RUNNING FROM RAM - IF THE FLASH HAS NO VALID BOOTLOADER, THE DEVICE WILL NOT BOOT AFTER A RESET.",
			"ram.title": "RUNNING FROM RAM",
			"ram.body": "THIS U-BOOT WAS LOADED INTO RAM BY THE PREVIOUS BOOT STAGE; NOTHING DONE HERE IS WRITTEN TO THE FLASH UNTIL IT IS WRITTEN EXPLICITLY.<br><br>IF THE BOOTLOADER IN THE FLASH IS MISSING, DAMAGED OR DOES NOT MATCH, THE DEVICE WILL NOT BOOT AGAIN AFTER A RESET - UPLOADING A FIRMWARE IMAGE ALONE DOES NOT FIX THAT.<br><br>BEFORE REBOOTING, MAKE SURE THE BOOTLOADER IN THE FLASH MATCHES YOUR FIRMWARE; WHEN IN DOUBT, FLASH THE <strong>BOOTLOADER</strong> (U-BOOT / FIP / BL2) AGAIN.",
			"ram.bootloader": "[ FLASH BOOTLOADER ]",
			"ram.dismiss": "[ I UNDERSTAND ]",
			"ram.reboot_confirm": "RUNNING FROM RAM: IF THE FLASH HAS NO VALID BOOTLOADER, REBOOTING NOW WILL BRICK IT - FLASH THE BOOTLOADER FIRST. REBOOT ANYWAY?",
			"lang.switch": "中文",
			"lang.label": "LANG",

			/* index.html */
			"index.fw_ubi": "\u25B6 UPGRADE SYSTEM FIRMWARE (UBI VOLUME) ",
			"index.fw_ubi_btn": "[ Upload Firmware ]",
			"index.uboot": "\u25B6 UPGRADE U-BOOT (MTD PARTITION)",
			"index.uboot_btn": "[ Upload U-Boot ]",
			"index.initramfs": "\u25B6 UPLOAD BOOT IMAGE (RAM BOOT)",
			"index.initramfs_btn": "[ Upload Image ]",
			"index.reboot_section": "\u25B6 SYSTEM REBOOT",
			"index.reboot_btn": "[ \u21BB Reboot Now ]",
			"index.reboot_failsafe_btn": "[ \u21BB Reboot To Failsafe ]",
			"index.boot_btn": "[ \u25B6 Boot System ]",
			"index.ticker": "\u26A0\uFE0F WARNING: THIS IS FAILSAFE RECOVERY MODE \u26A0\uFE0F DO NOT POWER OFF DURING FLASH \u26A0\uFE0F ALL DATA MAY BE LOST \u26A0\uFE0F PROCEED WITH CAUTION \u26A0\uFE0F AIROHA SEMICONDUCTOR \u26A0\uFE0F ",

			/* Upload confirmation (shown after AJAX upload on index) */
			"upload.confirm": "\u26A0\u00A0IF ALL INFORMATION ABOVE IS CORRECT, CLICK [PROCEED] TO BEGIN FLASH OPERATION.",
			"upload.warning": "\u00A0\u00A0DO NOT DISCONNECT POWER OR CLOSE THIS BROWSER WINDOW.",
			"upload.btn": "[ \u25B6\u25B6 PROCEED \u25C0\u25C0 ]",

			/* uboot.html */
			"uboot.title": "\u25B6 U-BOOT IMAGE CONFIRMATION [MTD PARTITION] ",
			"uboot.replace": "\u26A0\u00A0THIS WILL REPLACE THE U-BOOT PARTITION PERMANENTLY.",
			"uboot.brick": "\u26A0\u00A0CORRUPT U-BOOT = DEVICE BRICKED. PROCEED WITH EXTREME CAUTION.",
			"uboot.confirm": "\u25B8\u00A0IF ALL INFORMATION ABOVE IS CORRECT, CLICK [PROCEED].",
			"uboot.btn": "[ \u25B6\u25B6 FLASH U-BOOT \u25C0\u25C0 ]",
			"uboot.ticker": "\u26A0\uFE0F CRITICAL: U-BOOT PARTITION OVERWRITE \u26A0\uFE0F RISK OF BRICK: CRITICAL \u26A0\uFE0F ENSURE STABLE POWER SUPPLY \u26A0\uFE0F NO TAKESIES BACKSIES \u26A0\uFE0F ",

			/* booting.html (initramfs confirmation) */
			"booting.title": "\u25B6 RAM BOOT IMAGE CONFIRMATION ",
			"booting.ram_boot": "\u25B8\u00A0THIS WILL BOOT THE UPLOADED IMAGE DIRECTLY FROM RAM WITHOUT WRITING TO FLASH.",
			"booting.volatile": "\u25B8\u00A0CHANGES WILL BE DISCARDED ON NEXT REBOOT.",
			"booting.confirm": "\u25B8\u00A0IF ALL INFORMATION ABOVE IS CORRECT, CLICK [PROCEED].",
			"booting.btn": "[ \u25B6\u25B6 BOOT RAM \u25C0\u25C0 ]",
			"booting.ticker": "\u26A0\uFE0F RAM BOOT MODE ACTIVE \u26A0\uFE0F NO FLASH WRITE WILL OCCUR \u26A0\uFE0F VOLATILE: CHANGES LOST ON RESET \u26A0\uFE0F ",

			/* flashing.html */
			"flashing.progress": "\u25B6 UPGRADE IN PROGRESS",
			"flashing.writing": "\u00A0\u00A0WRITING TO FLASH MEMORY",
			"flashing.verifying": "\u00A0\u00A0VERIFYING CRC CHECKSUM",
			"flashing.not_responding": "\u00A0\u00A0THIS PAGE MAY BE IN NOT RESPONDING STATUS FOR A SHORT TIME.",
			"flashing.no_poweroff": "\u00A0\u00A0\u26A0\u00A0DO NOT POWER OFF DEVICE",
			"flashing.no_close": "\u00A0DO NOT CLOSE WINDOW",
			"flashing.ticker": "\u26A0\uFE0F FLASHING IN PROGRESS \u26A0\uFE0F DO NOT POWER OFF \u26A0\uFE0F DO NOT UNPLUG \u26A0\uFE0F WAIT FOR COMPLETION \u26A0\uFE0F BRICK RISK: ACTIVE \u26A0\uFE0F ",

			/* Success fragment (injected by flashing.html) */
			"success.title": "\u2588\u2588\u2588 UPGRADE COMPLETE! SYSTEM OK \u2588\u2588\u2588",
			"success.reboot": "\u00A0\u00A0>> SELECT REBOOT METHOD...",
			"success.crc": "\u00A0\u00A0>> FLASH CRC: PASS",
			"success.layout": "\u00A0\u00A0>> PARTITION LAYOUT: VALID",
			"success.checksum": "\u00A0\u00A0>> UBOOT CHECKSUM: VERIFIED",
			"success.status": "\u00A0\u00A0>> DEVICE STATUS: READY",
			"reboot.btn": "[ \u21BB REBOOT NOW ]",
			"reboot.failsafe_btn": "[ \u21BB REBOOT TO FAILSAFE ]",

			/* reboot.html */
			"reboot.progress": "\u25B6 SYSTEM REBOOT IN PROGRESS",
			"reboot.waiting": "\u00A0\u00A0WAITING FOR DEVICE TO COME BACK ONLINE",
			"reboot.hint": "\u00A0\u00A0IF DEVICE DOES NOT RESPOND WITHIN 60S, POWER-CYCLE MANUALLY.",
			"reboot.no_poweroff": "DO NOT POWER OFF DEVICE",
			"reboot.ticker": "\u26A0\uFE0F REBOOTING DEVICE \u26A0\uFE0F PLEASE WAIT \u26A0\uFE0F DEVICE WILL RETURN AUTOMATICALLY \u26A0\uFE0F ",

			/* Fail fragment / fail.html page */
			"fail.title": "\u2588\u2588\u2588 UPGRADE FAILED! SYSTEM ERROR \u2588\u2588\u2588",
			"fail.write": "\u00A0\u00A0>> FLASH WRITE: ABORTED",
			"fail.code": "\u00A0\u00A0>> ERROR CODE: 0xDEADBEEF",
			"fail.recovery": "\u00A0\u00A0>> RECOVERY: FALLBACK TO STOCK IMAGE",
			"fail.invalid": "\u00A0\u00A0THE UPLOADED FILE MAY BE INVALID OR CORRUPT.",
			"fail.retry": "\u00A0\u00A0PLEASE VERIFY THE FILE AND TRY AGAIN.",
			"fail.btn": "[ \u21BA RETURN TO RECOVERY MENU ]",
			"fail.ticker": "\u26A0\uFE0F UPGRADE FAILED \u26A0\uFE0F DEVICE NOT MODIFIED \u26A0\uFE0F SAFE TO RETRY \u26A0\uFE0F ",

			/* 404 page */
			"404.title": "\u25B6 PAGE NOT FOUND \u2014 RESOURCE MISSING ",
			"404.http": "HTTP/1.1 404 NOT_FOUND",
			"404.server": "Server: uboot-failsafe/2.4.1",
			"404.hint": "Hint: Go back to / (root)",
			"404.missing": "\u00A0\u00A0REQUESTED URL DOES NOT EXIST ON THIS DEVICE.",
			"404.void": "\u00A0\u00A0YOU ARE LOST IN THE VOID. RETURN TO BASE.",
			"404.btn": "[ \u21BA RETURN TO RECOVERY MENU ]",
			"404.ticker": "\u26A0\uFE0F 404 ERROR \u26A0\uFE0F PAGE NOT FOUND \u26A0\uFE0F BROKEN LINK OR TYPED URL \u26A0\uFE0F NAVIGATE BACK TO ROOT \u26A0\uFE0F ",

			/* File info labels */
			"fileinfo.md5": "MD5\u00A0:",
			"fileinfo.size": "SIZE:",
			"fileinfo.bytes": "BYTES"
		},

		zh: {
			/* 通用 */
			"banner.title": "AIROHA U-BOOT 系统恢复",
			/* RAM（串口恢复）会话警告 - 见 main.js 与 common/boot_mode.c */
			"ram.badge": "RAM",
			"ram.banner": "当前运行在内存中 —— 若闪存中没有可用的引导程序，重启后设备将无法启动。",
			"ram.title": "当前运行在内存中",
			"ram.body": "当前 U-Boot 是由上一级引导加载到内存中运行的，在显式写入之前，本次会话的任何内容都不会保存到闪存。<br><br>如果闪存中的引导程序缺失、损坏或不匹配，重启后设备就无法再启动，只上传固件并不能解决这个问题。<br><br>请在重启前确认闪存中的引导程序与固件匹配；不确定时，先重新刷一遍 <strong>引导程序</strong>（U-Boot / FIP / BL2）。",
			"ram.bootloader": "[ 刷写引导程序 ]",
			"ram.dismiss": "[ 我知道了 ]",
			"ram.reboot_confirm": "当前运行在内存中：若闪存中没有可用的引导程序，此时会导致设备无法启动 —— 请先刷写引导程序。仍要重启？",
			"lang.switch": "EN",
			"lang.label": "语言",

			/* index.html */
			"index.fw_ubi": "\u25B6 升级系统固件 (UBI 卷) ",
			"index.fw_ubi_btn": "[ 上传固件 ]",
			"index.uboot": "\u25B6 升级 U-Boot (MTD 分区)",
			"index.uboot_btn": "[ 上传 U-Boot ]",
			"index.initramfs": "\u25B6 上传启动镜像 (内存启动)",
			"index.initramfs_btn": "[ 上传镜像 ]",
			"index.reboot_section": "\u25B6 系统重启",
			"index.reboot_btn": "[ \u21BB 立即重启 ]",
			"index.reboot_failsafe_btn": "[ \u21BB 重启至 Failsafe ]",
			"index.boot_btn": "[ \u25B6 直接启动系统 ]",
			"index.ticker": "\u26A0\uFE0F 警告：此为故障安全恢复模式 \u26A0\uFE0F 刷写过程中请勿断电 \u26A0\uFE0F 所有数据可能丢失 \u26A0\uFE0F 请谨慎操作 \u26A0\uFE0F AIROHA 半导体 \u26A0\uFE0F ",

			/* 上传确认（AJAX 后在 index 上显示） */
			"upload.confirm": "\u26A0\u00A0如果以上信息正确，请点击 [继续] 开始刷写操作。",
			"upload.warning": "\u00A0\u00A0请勿断开电源或关闭此浏览器窗口。",
			"upload.btn": "[ \u25B6\u25B6 继 续 \u25C0\u25C0 ]",

			/* uboot.html */
			"uboot.title": "\u25B6 U-Boot 镜像确认 [MTD 分区] ",
			"uboot.replace": "\u26A0\u00A0此操作将永久替换 U-Boot 分区。",
			"uboot.brick": "\u26A0\u00A0U-Boot 损坏 = 设备变砖。请格外谨慎操作。",
			"uboot.confirm": "\u25B8\u00A0如果以上信息正确，请点击 [继续]。",
			"uboot.btn": "[ \u25B6\u25B6 刷写 U-BOOT \u25C0\u25C0 ]",
			"uboot.ticker": "\u26A0\uFE0F 危险：U-Boot 分区覆盖 \u26A0\uFE0F 变砖风险：极高 \u26A0\uFE0F 确保电源稳定 \u26A0\uFE0F 不可逆操作 \u26A0\uFE0F ",

			/* booting.html (initramfs 确认) */
			"booting.title": "\u25B6 内存启动镜像确认 ",
			"booting.ram_boot": "\u25B8\u00A0此操作将从内存直接启动上传的镜像，不写入闪存。",
			"booting.volatile": "\u25B8\u00A0更改将在下次重启时丢弃。",
			"booting.confirm": "\u25B8\u00A0如果以上信息正确，请点击 [继续]。",
			"booting.btn": "[ \u25B6\u25B6 内存启动 \u25C0\u25C0 ]",
			"booting.ticker": "\u26A0\uFE0F 内存启动模式已激活 \u26A0\uFE0F 不会写入闪存 \u26A0\uFE0F 易失性：重启后更改丢失 \u26A0\uFE0F ",

			/* flashing.html */
			"flashing.progress": "\u25B6 升级进行中",
			"flashing.writing": "\u00A0\u00A0正在写入闪存",
			"flashing.verifying": "\u00A0\u00A0正在验证 CRC 校验码",
			"flashing.not_responding": "\u00A0\u00A0此页面可能在短时间内无响应。",
			"flashing.no_poweroff": "\u00A0\u00A0\u26A0\u00A0请勿关闭设备电源",
			"flashing.no_close": "\u00A0请勿关闭窗口",
			"flashing.ticker": "\u26A0\uFE0F 正在刷写 \u26A0\uFE0F 请勿断电 \u26A0\uFE0F 请勿拔线 \u26A0\uFE0F 等待完成 \u26A0\uFE0F 变砖风险：激活中 \u26A0\uFE0F ",

			/* 成功片段 (flashing.html 注入) */
			"success.title": "\u2588\u2588\u2588 升级完成！系统正常 \u2588\u2588\u2588",
			"success.reboot": "\u00A0\u00A0>> 选择重启方式...",
			"success.crc": "\u00A0\u00A0>> 闪存 CRC：通过",
			"success.layout": "\u00A0\u00A0>> 分区布局：有效",
			"success.checksum": "\u00A0\u00A0>> UBOOT 校验码：已验证",
			"success.status": "\u00A0\u00A0>> 设备状态：就绪",
			"reboot.btn": "[ \u21BB 立即重启 ]",
			"reboot.failsafe_btn": "[ \u21BB 重启至 Failsafe ]",

			/* reboot.html */
			"reboot.progress": "\u25B6 系统重启中",
			"reboot.waiting": "\u00A0\u00A0等待设备重新上线",
			"reboot.hint": "\u00A0\u00A0如果设备 60 秒内无响应，请手动重新上电。",
			"reboot.no_poweroff": "请勿关闭设备电源",
			"reboot.ticker": "\u26A0\uFE0F 正在重启设备 \u26A0\uFE0F 请等待 \u26A0\uFE0F 设备将自动返回 \u26A0\uFE0F ",

			/* 失败片段 / fail.html 页面 */
			"fail.title": "\u2588\u2588\u2588 升级失败！系统错误 \u2588\u2588\u2588",
			"fail.write": "\u00A0\u00A0>> 闪存写入：已中止",
			"fail.code": "\u00A0\u00A0>> 错误代码：0xDEADBEEF",
			"fail.recovery": "\u00A0\u00A0>> 恢复：回退到出厂镜像",
			"fail.invalid": "\u00A0\u00A0上传的文件可能无效或损坏。",
			"fail.retry": "\u00A0\u00A0请验证文件后重试。",
			"fail.btn": "[ \u21BA 返回恢复菜单 ]",
			"fail.ticker": "\u26A0\uFE0F 升级失败 \u26A0\uFE0F 设备未修改 \u26A0\uFE0F 可安全重试 \u26A0\uFE0F ",

			/* 404 页面 */
			"404.title": "\u25B6 页面未找到 \u2014 资源缺失 ",
			"404.http": "HTTP/1.1 404 未找到",
			"404.server": "服务器：uboot-failsafe/2.4.1",
			"404.hint": "提示：返回 / (根目录)",
			"404.missing": "\u00A0\u00A0请求的 URL 在此设备上不存在。",
			"404.void": "\u00A0\u00A0你迷失在虚空中。请返回基地。",
			"404.btn": "[ \u21BA 返回恢复菜单 ]",
			"404.ticker": "\u26A0\uFE0F 404 错误 \u26A0\uFE0F 页面未找到 \u26A0\uFE0F 链接失效或 URL 错误 \u26A0\uFE0F 请导航回根目录 \u26A0\uFE0F ",

			/* 文件信息标签 */
			"fileinfo.md5": "MD5\u00A0:",
			"fileinfo.size": "大小:",
			"fileinfo.bytes": "字节"
		}
	};

	var KEY = "failsafe_lang";
	var cur = "en";

	function detect() {
		try {
			var s = localStorage.getItem(KEY);
			if (s && T[s]) return s;
		} catch (e) {}
		var n = (navigator.language || navigator.userLanguage || "en");
		return (n.indexOf("zh") === 0) ? "zh" : "en";
	}

	function t(k) {
		var d = T[cur] || T.en;
		return d[k] || (T.en[k] || k);
	}

	function setLang(l) {
		if (!T[l]) l = "en";
		cur = l;
		try { localStorage.setItem(KEY, l); } catch (e) {}
		apply();
	}

	function toggleLang() { setLang(cur === "en" ? "zh" : "en"); }

	/* Apply translations within root (or whole document).
	 * Supports:
	 *   data-i18n="key"             -> textContent, or value for INPUT
	 *   data-i18n-attr="attr:key"   -> set attribute attr to translated key
	 */
	function apply(root) {
		root = root || d;

		var nodes = root.querySelectorAll("[data-i18n]");
		for (var i = 0; i < nodes.length; i++) {
			var k = nodes[i].getAttribute("data-i18n");
			var v = t(k);
			if (!v || v === k) continue;
			if (nodes[i].tagName === "INPUT") nodes[i].value = v;
			else nodes[i].innerHTML = v;
		}

		var attrNodes = root.querySelectorAll("[data-i18n-attr]");
		for (var j = 0; j < attrNodes.length; j++) {
			var spec = attrNodes[j].getAttribute("data-i18n-attr");
			if (!spec) continue;
			var colon = spec.indexOf(":");
			if (colon < 0) continue;
			var attr = spec.substring(0, colon);
			var key2 = spec.substring(colon + 1);
			var val = t(key2);
			if (!val || val === key2) continue;
			attrNodes[j].setAttribute(attr, val);
		}

		var btn = document.getElementById("lang-switch");
		if (btn) {
			btn.hidden = Object.keys(T).length < 2;
			btn.textContent = t("lang.switch");
		}
	}

	function init() { cur = detect(); apply(); }

	var i18n = {
		translations: T,
		get lang() { return cur; },
		t: t,
		setLang: setLang,
		toggleLang: toggleLang,
		applyTranslations: apply,
		init: init
	};

	w.i18n = i18n;
	var d = document;

	if (d.readyState === "loading") d.addEventListener("DOMContentLoaded", init);
	else init();

})(window);
