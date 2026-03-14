#pragma once

#include <Arduino.h>

namespace web_ui {

static const char kPage[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Coffee Bridge</title>
<style>
  :root {
    color-scheme: light;
    --bg: #efe7dc;
    --bg-deep: #dcccb8;
    --panel: rgba(255, 249, 241, 0.92);
    --panel-strong: #fff9f2;
    --text: #1f1b17;
    --muted: #695d51;
    --line: rgba(71, 56, 40, 0.14);
    --accent: #2c6a4b;
    --accent-strong: #194630;
    --accent-soft: rgba(44, 106, 75, 0.12);
    --warn: #a5522a;
    --warn-soft: rgba(165, 82, 42, 0.12);
    --shadow: 0 18px 44px rgba(67, 48, 32, 0.14);
    --radius: 24px;
    --mono: "Iosevka", "SFMono-Regular", "Consolas", monospace;
    --sans: "Avenir Next", "Gill Sans", "Trebuchet MS", sans-serif;
  }

  * {
    box-sizing: border-box;
  }

  html, body {
    min-height: 100%;
  }

  body {
    margin: 0;
    color: var(--text);
    font: 15px/1.5 var(--sans);
    background:
      radial-gradient(circle at top left, rgba(255,255,255,0.55), transparent 32%),
      radial-gradient(circle at bottom right, rgba(44,106,75,0.18), transparent 24%),
      linear-gradient(145deg, var(--bg), var(--bg-deep));
  }

  body.modal-open {
    overflow: hidden;
  }

  a {
    color: inherit;
    text-decoration: none;
  }

  button,
  input,
  select,
  textarea {
    font: inherit;
  }

  button,
  .button-link {
    border: 0;
    border-radius: 999px;
    padding: 11px 16px;
    background: var(--accent);
    color: #fffdf9;
    cursor: pointer;
    transition: transform 0.12s ease, filter 0.12s ease, background 0.12s ease;
  }

  button:hover,
  .button-link:hover {
    filter: brightness(1.02);
    transform: translateY(-1px);
  }

  button.secondary,
  .button-link.secondary {
    background: rgba(31, 27, 23, 0.08);
    color: var(--text);
  }

  button.warn,
  .button-link.warn {
    background: var(--warn);
  }

  button.ghost,
  .button-link.ghost {
    background: transparent;
    border: 1px solid var(--line);
    color: var(--text);
  }

  button:disabled {
    opacity: 0.55;
    cursor: default;
    transform: none;
  }

  input,
  select,
  textarea {
    width: 100%;
    border: 1px solid var(--line);
    border-radius: 16px;
    padding: 12px 14px;
    background: rgba(255, 255, 255, 0.85);
    color: var(--text);
  }

  textarea {
    min-height: 120px;
    resize: vertical;
    font-family: var(--mono);
  }

  pre,
  code {
    font-family: var(--mono);
  }

  pre {
    margin: 0;
    padding: 14px;
    border-radius: 18px;
    background: #231d18;
    color: #f6efe6;
    overflow: auto;
    white-space: pre-wrap;
    word-break: break-word;
  }

  .shell {
    max-width: 1480px;
    margin: 0 auto;
    padding: 24px;
  }

  .masthead {
    display: flex;
    justify-content: space-between;
    gap: 20px;
    align-items: flex-start;
    margin-bottom: 18px;
  }

  .eyebrow {
    margin: 0 0 8px;
    font-size: 12px;
    letter-spacing: 0.22em;
    text-transform: uppercase;
    color: var(--muted);
  }

  .masthead h1 {
    margin: 0;
    font-size: clamp(32px, 5vw, 52px);
    line-height: 0.96;
    letter-spacing: -0.04em;
    max-width: 12ch;
  }

  .masthead p {
    margin: 12px 0 0;
    color: var(--muted);
    max-width: 48ch;
  }

  .masthead-meta {
    display: grid;
    gap: 12px;
    justify-items: end;
    min-width: 250px;
  }

  .build-pill,
  .flash,
  .panel,
  .stat-card,
  .machine-card,
  .hero-card,
  .diag-block {
    backdrop-filter: blur(16px);
  }

  .build-pill {
    padding: 10px 14px;
    border-radius: 999px;
    background: rgba(255, 249, 241, 0.72);
    border: 1px solid rgba(255, 255, 255, 0.6);
    box-shadow: var(--shadow);
    color: var(--muted);
    font-size: 13px;
  }

  .flash {
    margin-bottom: 18px;
    padding: 14px 16px;
    border-radius: 18px;
    background: rgba(255, 249, 241, 0.85);
    border: 1px solid var(--line);
    box-shadow: var(--shadow);
  }

  .flash.ok {
    border-color: rgba(44, 106, 75, 0.28);
    background: rgba(226, 242, 232, 0.95);
  }

  .flash.error {
    border-color: rgba(165, 82, 42, 0.28);
    background: rgba(252, 234, 225, 0.96);
  }

  .layout {
    display: grid;
    grid-template-columns: 320px minmax(0, 1fr);
    gap: 18px;
    align-items: start;
  }

  .sidebar,
  .content {
    min-width: 0;
  }

  .panel,
  .hero-card,
  .diag-block {
    background: var(--panel);
    border: 1px solid rgba(255, 255, 255, 0.72);
    box-shadow: var(--shadow);
    border-radius: var(--radius);
    padding: 20px;
  }

  .panel + .panel,
  .content > * + * {
    margin-top: 18px;
  }

  .sidebar-title,
  .section-title,
  .hero-card h2,
  .machine-hero h2 {
    margin: 0 0 12px;
    font-size: 21px;
    letter-spacing: -0.03em;
  }

  .muted {
    color: var(--muted);
  }

  .tiny {
    font-size: 13px;
  }

  .sidebar-nav,
  .subnav {
    display: grid;
    gap: 8px;
  }

  .nav-link,
  .subnav a {
    display: flex;
    justify-content: space-between;
    gap: 12px;
    align-items: center;
    padding: 12px 14px;
    border-radius: 18px;
    color: var(--text);
    background: rgba(255, 255, 255, 0.42);
    border: 1px solid transparent;
  }

  .nav-link.active,
  .subnav a.active {
    background: var(--accent-soft);
    border-color: rgba(44, 106, 75, 0.16);
  }

  .sidebar-machines {
    display: grid;
    gap: 10px;
  }

  .sidebar-machine {
    display: block;
    padding: 12px 14px;
    border-radius: 18px;
    background: rgba(255, 255, 255, 0.55);
    border: 1px solid rgba(255, 255, 255, 0.62);
  }

  .sidebar-machine.active {
    background: rgba(44, 106, 75, 0.16);
    border-color: rgba(44, 106, 75, 0.28);
  }

  .badge-row,
  .row {
    display: flex;
    flex-wrap: wrap;
    gap: 10px;
    align-items: center;
  }

  .badge {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    padding: 6px 10px;
    border-radius: 999px;
    background: rgba(31, 27, 23, 0.08);
    color: var(--text);
    font-size: 13px;
  }

  .badge.good {
    background: rgba(44, 106, 75, 0.14);
    color: var(--accent-strong);
  }

  .badge.warn {
    background: var(--warn-soft);
    color: #7c391a;
  }

  .grid {
    display: grid;
    gap: 16px;
  }

  .grid.two {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }

  .grid.three {
    grid-template-columns: repeat(3, minmax(0, 1fr));
  }

  .stats-grid,
  .machines-grid,
  .recipes-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(240px, 1fr));
    gap: 14px;
  }

  .stat-card,
  .machine-card {
    padding: 18px;
    border-radius: 22px;
    background: var(--panel-strong);
    border: 1px solid rgba(255, 255, 255, 0.7);
    box-shadow: var(--shadow);
  }

  .machine-card {
    display: grid;
    gap: 14px;
  }

  .machine-card h3,
  .stat-card h3,
  .recipe-card h3 {
    margin: 0;
    font-size: 20px;
    letter-spacing: -0.03em;
  }

  .machine-card .meta,
  .recipe-card .meta {
    display: grid;
    gap: 4px;
    color: var(--muted);
    font-size: 14px;
  }

  .hero-card {
    overflow: hidden;
    position: relative;
  }

  .hero-card::after {
    content: "";
    position: absolute;
    inset: auto -20% -45% auto;
    width: 240px;
    height: 240px;
    border-radius: 50%;
    background: rgba(44, 106, 75, 0.12);
    pointer-events: none;
  }

  .machine-hero {
    display: grid;
    gap: 16px;
  }

  .machine-hero h2 {
    margin-bottom: 4px;
    font-size: clamp(28px, 4vw, 40px);
  }

  .machine-subtitle {
    color: var(--muted);
    font-size: 15px;
  }

  .progress {
    height: 10px;
    border-radius: 999px;
    background: rgba(31, 27, 23, 0.08);
    overflow: hidden;
  }

  .progress > span {
    display: block;
    height: 100%;
    width: 0;
    background: linear-gradient(90deg, var(--accent), #8ba83f);
    transition: width 0.24s ease;
  }

  .recipe-card,
  .setting-row,
  .scan-row {
    padding: 16px 18px;
    border-radius: 22px;
    background: rgba(255, 255, 255, 0.72);
    border: 1px solid rgba(255, 255, 255, 0.68);
  }

  .recipe-card {
    display: grid;
    gap: 10px;
  }

  .setting-row {
    display: grid;
    gap: 12px;
  }

  .scan-list,
  .recipe-list {
    display: grid;
    gap: 12px;
  }

  .scan-row strong,
  .setting-row strong {
    font-size: 17px;
    letter-spacing: -0.02em;
  }

  .scan-row.featured {
    border-color: rgba(44, 106, 75, 0.24);
    background: linear-gradient(180deg, rgba(232, 244, 236, 0.92), rgba(255, 255, 255, 0.72));
  }

  .label {
    display: block;
    margin-bottom: 8px;
    font-weight: 600;
  }

  .field-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(210px, 1fr));
    gap: 12px;
  }

  .value-list {
    display: grid;
    gap: 10px;
  }

  .value-item {
    display: flex;
    justify-content: space-between;
    gap: 12px;
    padding: 10px 12px;
    border-radius: 16px;
    background: rgba(31, 27, 23, 0.05);
  }

  .value-item span:last-child {
    text-align: right;
    font-weight: 600;
  }

  .empty {
    padding: 18px;
    border-radius: 22px;
    background: rgba(255, 255, 255, 0.5);
    border: 1px dashed var(--line);
    color: var(--muted);
  }

  .diag-block {
    display: grid;
    gap: 12px;
  }

  .diag-block h3 {
    margin: 0;
    font-size: 18px;
    letter-spacing: -0.02em;
  }

  .stack {
    display: grid;
    gap: 12px;
  }

  details {
    border-radius: 18px;
    background: rgba(31, 27, 23, 0.05);
    padding: 12px 14px;
  }

  summary {
    cursor: pointer;
    font-weight: 600;
  }

  .modal-backdrop {
    position: fixed;
    inset: 0;
    z-index: 40;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 24px;
    background: rgba(27, 22, 18, 0.34);
    backdrop-filter: blur(10px);
  }

  .modal-card {
    width: min(680px, 100%);
    display: grid;
    gap: 18px;
    padding: 24px;
    border-radius: 30px;
    background: rgba(255, 249, 241, 0.97);
    border: 1px solid rgba(255, 255, 255, 0.76);
    box-shadow: 0 28px 60px rgba(45, 31, 21, 0.22);
  }

  .modal-card h2 {
    margin: 0;
    font-size: clamp(26px, 4vw, 34px);
    line-height: 0.98;
    letter-spacing: -0.04em;
  }

  .modal-card p {
    margin: 0;
  }

  .modal-body {
    display: grid;
    gap: 14px;
  }

  .modal-actions {
    display: flex;
    flex-wrap: wrap;
    justify-content: flex-end;
    gap: 10px;
  }

  @media (max-width: 1100px) {
    .layout {
      grid-template-columns: 1fr;
    }

    .masthead {
      grid-template-columns: 1fr;
      display: grid;
    }

    .masthead-meta {
      justify-items: start;
    }
  }

  @media (max-width: 720px) {
    .shell {
      padding: 16px;
    }

    .modal-backdrop {
      align-items: flex-end;
      padding: 16px;
    }

    .modal-card {
      padding: 20px;
      border-radius: 26px;
    }

    .grid.two,
    .grid.three {
      grid-template-columns: 1fr;
    }

    .value-item {
      display: grid;
    }
  }
</style>
</head>
<body>
  <div class="shell">
    <header class="masthead">
      <div>
        <p class="eyebrow">Coffee Bridge</p>
        <h1>Saved Coffee Machines</h1>
        <p>Pair nearby supported coffee machines, remember them on the bridge, watch presence and signal strength, and operate recipes, settings, statistics, and diagnostics from one place.</p>
      </div>
      <div class="masthead-meta">
        <div class="build-pill">Build )HTML" __DATE__ " " __TIME__ R"HTML(</div>
        <div class="badge-row" id="header-badges"></div>
      </div>
    </header>

    <div id="flash" class="flash">Loading bridge status…</div>

    <div class="layout">
      <aside class="sidebar" id="sidebar"></aside>
      <main class="content" id="app"></main>
    </div>
  </div>
  <div id="modal-root"></div>

<script>
  const SETTING_OPTIONS = {
    water_hardness: ["soft", "medium", "hard", "very hard"],
    temperature: ["normal", "high", "max", "individual"],
    off_rinse: ["off", "on"],
    auto_off: ["10 min", "30 min", "1 h", "2 h", "4 h", "6 h", "8 h", "10 h", "12 h", "14 h", "16 h", "off"],
    profile: ["dynamic", "constant", "intense", "quick", "individual"],
    coffee_temperature: ["off", "on", "normal", "high", "max", "individual"],
    water_temperature: ["normal", "high", "max", "individual"],
    milk_temperature: ["normal", "high", "hot", "max", "individual"],
    milk_foam_temperature: ["warm", "max", "individual"],
    power_on_rinse: ["off", "on"],
    power_on_frother_time: ["10 min", "20 min", "30 min", "40 min"]
  };

  const PROFILE_OPTIONS = [
    { value: 0, label: "dynamic" },
    { value: 1, label: "constant" },
    { value: 2, label: "intense" },
    { value: 3, label: "individual" },
    { value: 4, label: "quick" }
  ];

  const TEMPERATURE_OPTIONS = [
    { value: 0, label: "normal" },
    { value: 1, label: "high" },
    { value: 2, label: "max" },
    { value: 3, label: "individual" },
    { value: 4, label: "hot" }
  ];

  const ON_OFF_OPTIONS = [
    { value: 0, label: "off" },
    { value: 1, label: "on" }
  ];

  const PROTOCOL_API = "/api/protocol";
  const HISTORY_PAGE_SIZE = 20;
  const STATS_HISTORY_PAGE_SIZE = 20;

  const state = {
    flash: { message: "Loading bridge status…", kind: "" },
    status: null,
    machines: [],
    scanResults: [],
    showCoffeeMachines: true,
    probedMachine: null,
    modal: null,
    machineCache: {},
    diagnostics: {
      output: null,
      logs: null
    }
  };

  const app = document.getElementById("app");
  const sidebar = document.getElementById("sidebar");
  const flash = document.getElementById("flash");
  const headerBadges = document.getElementById("header-badges");
  const modalRoot = document.getElementById("modal-root");

  function escapeHtml(value) {
    return String(value ?? "")
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;")
      .replace(/'/g, "&#39;");
  }

  function pretty(value) {
    try {
      return JSON.stringify(value ?? {}, null, 2);
    } catch (error) {
      return String(value);
    }
  }

  function formatByteHex(value) {
    const byte = Math.max(0, Math.min(255, Number(value ?? 0) || 0));
    return `0x${byte.toString(16).toUpperCase().padStart(2, "0")}`;
  }

  function formatByteBits(value) {
    const byte = Math.max(0, Math.min(255, Number(value ?? 0) || 0));
    return byte.toString(2).padStart(8, "0");
  }

  function parsePayload(text) {
    if (!text) {
      return null;
    }
    try {
      return JSON.parse(text);
    } catch (error) {
      return text;
    }
  }

  async function api(path, options = {}) {
    const request = {
      method: options.method || (options.json !== undefined ? "POST" : "GET"),
      headers: { ...(options.headers || {}) }
    };

    request.headers["X-Bridge-Time-Unix-Ms"] = String(Date.now());

    if (options.json !== undefined) {
      request.body = JSON.stringify(options.json);
      request.headers["Content-Type"] = "application/json";
    } else if (options.body !== undefined) {
      request.body = options.body;
    }

    const response = await fetch(path, request);
    const text = await response.text();
    const data = parsePayload(text);
    if (!response.ok) {
      const message = typeof data === "object" && data && data.error ? data.error : (typeof data === "string" ? data : `${response.status} ${response.statusText}`);
      const error = new Error(message);
      error.status = response.status;
      error.data = data;
      throw error;
    }
    return data;
  }

  function setFlash(message, kind = "") {
    state.flash = { message, kind };
    flash.className = `flash${kind ? ` ${kind}` : ""}`;
    flash.textContent = message;
  }

  function renderModal() {
    if (!modalRoot) {
      return;
    }
    if (!state.modal) {
      document.body.classList.remove("modal-open");
      modalRoot.innerHTML = "";
      return;
    }

    document.body.classList.add("modal-open");
    const modal = state.modal;
    const confirmClassName = modal.confirmClass ? ` ${modal.confirmClass}` : "";
    modalRoot.innerHTML = `
      <div class="modal-backdrop" role="presentation">
        <section class="modal-card" role="dialog" aria-modal="true" aria-labelledby="modal-title" aria-describedby="modal-body">
          <div class="stack" style="gap:8px;">
            <h2 id="modal-title">${escapeHtml(modal.title || "Confirm action")}</h2>
            ${modal.subtitle ? `<p class="muted" id="modal-subtitle">${escapeHtml(modal.subtitle)}</p>` : ""}
          </div>
          <div class="modal-body" id="modal-body">${modal.bodyHtml || ""}</div>
          <div class="modal-actions">
            <button type="button" class="secondary" data-action="modal-cancel">${escapeHtml(modal.cancelLabel || "Cancel")}</button>
            <button type="button" class="${`modal-confirm${confirmClassName}`.trim()}" data-action="modal-confirm" data-role="modal-confirm">${escapeHtml(modal.confirmLabel || "Confirm")}</button>
          </div>
        </section>
      </div>
    `;

    const focusTarget = modalRoot.querySelector('[data-role="modal-confirm"]');
    if (focusTarget && typeof focusTarget.focus === "function") {
      setTimeout(() => focusTarget.focus(), 0);
    }
  }

  function closeModal(result = false) {
    if (!state.modal) {
      return;
    }
    const resolve = state.modal.resolve;
    state.modal = null;
    renderModal();
    if (typeof resolve === "function") {
      resolve(result);
    }
  }

  function openConfirmModal(options = {}) {
    return new Promise((resolve) => {
      if (state.modal && typeof state.modal.resolve === "function") {
        state.modal.resolve(false);
      }
      state.modal = {
        title: options.title || "Confirm action",
        subtitle: options.subtitle || "",
        bodyHtml: options.bodyHtml || "",
        confirmLabel: options.confirmLabel || "Confirm",
        cancelLabel: options.cancelLabel || "Cancel",
        confirmClass: options.confirmClass || "",
        resolve
      };
      renderModal();
    });
  }

  function route() {
    const cleaned = location.hash.replace(/^#\/?/, "");
    const parts = cleaned ? cleaned.split("/").filter(Boolean) : [];
    if (!parts.length) {
      return { name: "dashboard" };
    }
    if (parts[0] === "add") {
      return { name: "add" };
    }
    if (parts[0] === "system") {
      return { name: "system" };
    }
    if (parts[0] === "machine" && parts[1]) {
      const serial = decodeURIComponent(parts[1]);
      if (parts[2] === "standard" && parts[3]) {
        return { name: "standard-recipe-detail", serial, selector: parts[3] };
      }
      if (parts[2] === "recipe" && parts[3]) {
        return { name: "recipe-detail", serial, slot: parts[3] };
      }
      return { name: "machine", serial, section: parts[2] || "recipes" };
    }
    return { name: "dashboard" };
  }

  function machineHref(serial, section = "recipes") {
    return `#/machine/${encodeURIComponent(serial)}/${section}`;
  }

  function machineRecipeHref(serial, slot) {
    return `#/machine/${encodeURIComponent(serial)}/recipe/${slot}`;
  }

  function machineStandardRecipeHref(serial, selector) {
    return `#/machine/${encodeURIComponent(serial)}/standard/${selector}`;
  }

  function isRouteActive(test) {
    const current = route();
    if (test === "dashboard") {
      return current.name === "dashboard";
    }
    if (test === "add") {
      return current.name === "add";
    }
    if (test === "system") {
      return current.name === "system";
    }
    return false;
  }

  function formatAgo(ms) {
    if (!ms) {
      return "offline";
    }
    const base = Number(state.status?.uptimeMs || state.status?.lastScanAtMs || 0);
    if (!base || ms > base) {
      return "just now";
    }
    const seconds = Math.max(0, Math.round((base - ms) / 1000));
    if (seconds < 15) {
      return "just now";
    }
    if (seconds < 60) {
      return `${seconds}s ago`;
    }
    const minutes = Math.round(seconds / 60);
    if (minutes < 60) {
      return `${minutes}m ago`;
    }
    const hours = Math.round(minutes / 60);
    return `${hours}h ago`;
  }

  function formatBytes(bytes) {
    const value = Math.max(0, Number(bytes || 0));
    if (value >= 1024 * 1024) {
      return `${(value / (1024 * 1024)).toFixed(value >= 10 * 1024 * 1024 ? 0 : 1)} MiB`;
    }
    if (value >= 1024) {
      return `${(value / 1024).toFixed(value >= 10 * 1024 ? 0 : 1)} KiB`;
    }
    return `${Math.round(value)} B`;
  }

  function timeModeLabel(mode) {
    return String(mode || "ntp") === "no_time" ? "No time" : "NTP";
  }

  function timeModeHelp(mode) {
    if (String(mode || "ntp") === "no_time") {
      return "No time mode disables NTP. The bridge will only learn time from an HTTP client request. After each boot, at least one HTTP request must reach the bridge before history can store real timestamps. If nothing seeds the clock, uptime values will be stored instead.";
    }
    return "NTP is requested whenever Wi-Fi connects. The saved server list is used in order until one answers.";
  }

  function timeSourceLabel(status) {
    const source = String(status?.timeSource || "");
    if (source === "ntp") {
      return "NTP";
    }
    if (source === "client") {
      return "Client request";
    }
    if (source === "restored") {
      return "Warm reboot restore";
    }
    return status?.timeAvailable ? "Clock available" : "No clock yet";
  }

  function ntpAttemptLabel(status) {
    if (String(status?.timeConfig?.mode || "ntp") === "no_time") {
      return "disabled";
    }
    return status?.timeLastAttemptMs ? formatAgo(status.timeLastAttemptMs) : "not yet";
  }

  function ntpSuccessLabel(status) {
    if (String(status?.timeConfig?.mode || "ntp") === "no_time") {
      return "disabled";
    }
    if (status?.timeLastSuccessIsoUtc) {
      return String(status.timeLastSuccessIsoUtc);
    }
    return "not yet";
  }

  function ntpDiagnosticLabel(status) {
    if (String(status?.timeConfig?.mode || "ntp") === "no_time") {
      return "NTP disabled";
    }
    return String(status?.ntpDiagnosticMessage || "Waiting for the first NTP attempt");
  }

  function ntpDiagnosticTargetLabel(status) {
    const server = String(status?.ntpDiagnosticServer || "");
    const address = String(status?.ntpDiagnosticAddress || "");
    if (server && address) {
      return `${server} (${address})`;
    }
    return server || address || "";
  }

  function applyTimeModeFormState(form, mode) {
    if (!(form instanceof HTMLFormElement)) {
      return;
    }
    const noTime = String(mode || "ntp") === "no_time";
    ["ntpServerPrimary", "ntpServerSecondary", "ntpServerTertiary"].forEach((name) => {
      const input = form.elements[name];
      if (input instanceof HTMLInputElement) {
        input.disabled = noTime;
      }
    });
    const warning = form.querySelector('[data-role="time-mode-warning"]');
    if (warning instanceof HTMLElement) {
      warning.textContent = timeModeHelp(mode);
      warning.className = noTime ? "empty" : "muted";
    }
  }

  function percentOf(value, max) {
    const numerator = Math.max(0, Number(value || 0));
    const denominator = Math.max(1, Number(max || 0));
    return Math.max(0, Math.min(100, (numerator / denominator) * 100));
  }

  function machinePresenceBadge(machine) {
    return machine.online
      ? `<span class="badge good">Online${machine.lastSeenRssi ? ` · ${machine.lastSeenRssi} dBm` : ""}</span>`
      : `<span class="badge warn">Offline</span>`;
  }

  function statusBadges(status) {
    if (!status) {
      return "";
    }
    const items = [
      status.staConnected ? `<span class="badge good">Wi-Fi ${escapeHtml(status.staSsid || "")}</span>` : `<span class="badge warn">AP only</span>`,
      `<span class="badge">${status.savedMachineCount || 0} saved</span>`,
      `<span class="badge">${status.supportedDeviceCount || 0} nearby supported</span>`
    ];
    return items.join("");
  }

  function findMachine(serial) {
    return state.machines.find((machine) => String(machine.serial).toLowerCase() === String(serial).toLowerCase()) || null;
  }

  function getMachineCache(serial) {
    if (!state.machineCache[serial]) {
      state.machineCache[serial] = {};
    }
    return state.machineCache[serial];
  }

  function primeSavedRecipeDetails(serial, data) {
    const cache = getMachineCache(serial);
    cache.recipeDetails = cache.recipeDetails || {};
    const source = data?.source || "live";
    const cached = !!data?.cached;
    (data?.recipes || []).forEach((recipe) => {
      if (!recipe || recipe.ok === false || recipe.slot === undefined) {
        return;
      }
      cache.recipeDetails[String(recipe.slot)] = { ok: true, source, cached, recipe };
    });
  }

  function mergeSavedRecipeDetail(serial, slot, data) {
    const cache = getMachineCache(serial);
    cache.recipeDetails = cache.recipeDetails || {};
    cache.recipeDetails[String(slot)] = data;
    if (!data?.recipe || !Array.isArray(cache.savedRecipes?.recipes)) {
      return;
    }
    const index = cache.savedRecipes.recipes.findIndex((item) => Number(item?.slot) === Number(slot));
    if (index >= 0) {
      cache.savedRecipes.recipes[index] = data.recipe;
    } else {
      cache.savedRecipes.recipes.push(data.recipe);
    }
  }

  function applyStandardRecipeRefresh(serial, data) {
    const cache = getMachineCache(serial);
    cache.standardRecipeDetails = cache.standardRecipeDetails || {};
    (data?.recipes || []).forEach((recipe) => {
      if (!recipe || recipe.ok === false || recipe.selector === undefined) {
        return;
      }
      cache.standardRecipeDetails[String(recipe.selector)] = {
        ok: true,
        source: data?.source || "live",
        cached: !!data?.cached,
        recipe
      };
    });
  }

  async function refreshCore() {
    const [status, machinesResponse] = await Promise.all([
      api("/api/status"),
      api("/api/machines")
    ]);

    state.status = status;
    state.machines = (machinesResponse.machines || []).slice().sort((a, b) => {
      if (a.online !== b.online) {
        return a.online ? -1 : 1;
      }
      return String(a.alias || a.modelName || a.serial).localeCompare(String(b.alias || b.modelName || b.serial));
    });
  }

  async function loadMachineSummary(serial, force = false) {
    const cache = getMachineCache(serial);
    if (!force && cache.summary) {
      return cache.summary;
    }
    cache.summary = await api(`/api/machines/${encodeURIComponent(serial)}/summary`);
    return cache.summary;
  }

  async function loadMachineRecipes(serial, force = false) {
    const cache = getMachineCache(serial);
    if (!force && cache.recipes) {
      return cache.recipes;
    }
    cache.recipes = await api(`/api/machines/${encodeURIComponent(serial)}/recipes`);
    return cache.recipes;
  }

  async function loadSavedRecipes(serial, force = false) {
    const cache = getMachineCache(serial);
    if (!force && cache.savedRecipes) {
      return cache.savedRecipes;
    }
    const suffix = force ? "?refresh=1" : "";
    cache.savedRecipes = await api(`/api/machines/${encodeURIComponent(serial)}/mycoffee${suffix}`);
    primeSavedRecipeDetails(serial, cache.savedRecipes);
    return cache.savedRecipes;
  }

  async function loadRecipeDetail(serial, slot, force = false) {
    const cache = getMachineCache(serial);
    cache.recipeDetails = cache.recipeDetails || {};
    if (!force && cache.recipeDetails[slot]) {
      return cache.recipeDetails[slot];
    }
    const suffix = force ? "?refresh=1" : "";
    cache.recipeDetails[slot] = await api(`/api/machines/${encodeURIComponent(serial)}/mycoffee/${slot}${suffix}`);
    mergeSavedRecipeDetail(serial, slot, cache.recipeDetails[slot]);
    return cache.recipeDetails[slot];
  }

  async function loadStandardRecipeDetail(serial, selector, force = false) {
    const cache = getMachineCache(serial);
    cache.standardRecipeDetails = cache.standardRecipeDetails || {};
    if (!force && cache.standardRecipeDetails[selector]) {
      return cache.standardRecipeDetails[selector];
    }
    const suffix = force ? "?refresh=1" : "";
    cache.standardRecipeDetails[selector] = await api(`/api/machines/${encodeURIComponent(serial)}/recipes/${selector}${suffix}`);
    return cache.standardRecipeDetails[selector];
  }

  async function loadMachineStats(serial, force = false) {
    const cache = getMachineCache(serial);
    if (!force && cache.stats) {
      return cache.stats;
    }
    cache.stats = await api(`/api/machines/${encodeURIComponent(serial)}/stats`);
    return cache.stats;
  }

  async function loadMachineStatsHistory(serial, force = false, offsetOverride = null) {
    const cache = getMachineCache(serial);
    const offset = Math.max(0, Number(offsetOverride ?? cache.statsHistoryOffset ?? 0));
    if (!force && cache.statsHistory && Number(cache.statsHistory.offset || 0) === offset &&
        Number(cache.statsHistory.limit || STATS_HISTORY_PAGE_SIZE) === STATS_HISTORY_PAGE_SIZE) {
      return cache.statsHistory;
    }
    cache.statsHistoryOffset = offset;
    cache.statsHistory = await api(`/api/machines/${encodeURIComponent(serial)}/stats/history?limit=${STATS_HISTORY_PAGE_SIZE}&offset=${offset}`);
    cache.statsHistoryOffset = Number(cache.statsHistory?.offset || 0);
    return cache.statsHistory;
  }

  async function loadMachineSettings(serial, force = false) {
    const cache = getMachineCache(serial);
    if (!force && cache.settings) {
      return cache.settings;
    }
    cache.settings = await api(`/api/machines/${encodeURIComponent(serial)}/settings`);
    return cache.settings;
  }

  async function loadMachineFeatures(serial, force = false) {
    const cache = getMachineCache(serial);
    if (!force && cache.features) {
      return cache.features;
    }
    cache.features = await api(`/api/machines/${encodeURIComponent(serial)}/features`);
    return cache.features;
  }

  async function loadMachineHistory(serial, force = false, offsetOverride = null) {
    const cache = getMachineCache(serial);
    const offset = Math.max(0, Number(offsetOverride ?? cache.historyOffset ?? 0));
    if (!force && cache.history && Number(cache.history.offset || 0) === offset &&
        Number(cache.history.limit || HISTORY_PAGE_SIZE) === HISTORY_PAGE_SIZE) {
      return cache.history;
    }
    cache.historyOffset = offset;
    cache.history = await api(`/api/machines/${encodeURIComponent(serial)}/history?limit=${HISTORY_PAGE_SIZE}&offset=${offset}`);
    cache.historyOffset = Number(cache.history?.offset || 0);
    return cache.history;
  }

  function invalidateMachine(serial) {
    delete state.machineCache[serial];
  }

  function looksLikeCoffeeMachine(device) {
    if (!device) {
      return false;
    }
    if (device.likelySupported || device.advertisedSupportedService) {
      return true;
    }
    const name = String(device.name || "").toLowerCase();
    return /nivona|nicr|eugster|coffee|cafe/.test(name);
  }

  function formatStatusCode(label, code) {
    if (label && label.length) {
      return `${label} (${code ?? "-"})`;
    }
    return `${code ?? "-"}`;
  }

  function statusSuggestsHostConfirm(status) {
    if (!status) {
      return false;
    }
    if (status.hostConfirmSuggested) {
      return true;
    }
    return Number(status.message || 0) === 11 || Number(status.message || 0) === 20;
  }

  function hostConfirmTitle(status) {
    if (Number(status?.message || 0) === 20) {
      return "Confirm flush prompt";
    }
    if (Number(status?.message || 0) === 11) {
      return "Confirm workflow prompt";
    }
    return "Confirm machine prompt";
  }

  function renderSidebar() {
    const current = route();
    sidebar.innerHTML = `
      <section class="panel">
        <h2 class="sidebar-title">Bridge</h2>
        <div class="sidebar-nav">
          <a class="nav-link ${isRouteActive("dashboard") ? "active" : ""}" href="#/dashboard">
            <span>Dashboard</span>
            <span class="muted tiny">${state.machines.length}</span>
          </a>
          <a class="nav-link ${isRouteActive("add") ? "active" : ""}" href="#/add">
            <span>Add coffee machine</span>
            <span class="muted tiny">${state.status?.supportedDeviceCount || 0} nearby</span>
          </a>
          <a class="nav-link ${isRouteActive("system") ? "active" : ""}" href="#/system">
            <span>System</span>
            <span class="muted tiny">${state.status?.staConnected ? "Wi-Fi" : "AP"}</span>
          </a>
        </div>
      </section>

      <section class="panel">
        <h2 class="sidebar-title">Remembered</h2>
        <div class="sidebar-machines">
          ${state.machines.length ? state.machines.map((machine) => `
            <a class="sidebar-machine ${current.serial && current.serial === machine.serial ? "active" : ""}" href="${machineHref(machine.serial, "recipes")}">
              <div class="row" style="justify-content:space-between;">
                <strong>${escapeHtml(machine.alias || machine.modelName || machine.serial)}</strong>
                ${machine.online ? '<span class="badge good">online</span>' : '<span class="badge warn">offline</span>'}
              </div>
              <div class="tiny muted">${escapeHtml(machine.modelName || machine.model || "Unknown model")}</div>
              <div class="tiny muted">${escapeHtml(machine.serial)}</div>
            </a>
          `).join("") : '<div class="empty">No saved machines yet. Start with <a href="#/add"><strong>Add coffee machine</strong></a>.</div>'}
        </div>
      </section>

      <section class="panel">
        <h2 class="sidebar-title">Live bridge</h2>
        <div class="value-list">
          <div class="value-item"><span>Station</span><span>${state.status?.staConnected ? escapeHtml(state.status.staIp || state.status.staSsid || "connected") : "offline"}</span></div>
          <div class="value-item"><span>AP</span><span>${escapeHtml(state.status?.apIp || "")}</span></div>
          <div class="value-item"><span>Scan</span><span>${state.status?.lastScanResultCount ?? 0} devices</span></div>
          <div class="value-item"><span>Protocol sessions</span><span>${state.status?.protocolSessionCount ?? 0}</span></div>
        </div>
      </section>
    `;

    headerBadges.innerHTML = statusBadges(state.status);
  }

  function sectionShell(title, subtitle, body) {
    return `
      <section class="hero-card">
        <h2>${escapeHtml(title)}</h2>
        <p class="muted">${escapeHtml(subtitle)}</p>
      </section>
      ${body}
    `;
  }

  function renderDashboard() {
    const machineCards = state.machines.length
      ? `<section class="machines-grid">
          ${state.machines.map((machine) => `
            <article class="machine-card">
              <div class="row" style="justify-content:space-between;align-items:flex-start;">
                <div>
                  <h3>${escapeHtml(machine.alias || machine.modelName || machine.serial)}</h3>
                  <div class="meta">
                    <span>${escapeHtml(machine.modelName || machine.model || "Unknown model")}</span>
                    <span>${escapeHtml(machine.serial)}</span>
                  </div>
                </div>
                ${machinePresenceBadge(machine)}
              </div>
              <div class="row">
                <span class="badge">${escapeHtml(machine.familyKey || "unknown family")}</span>
                ${machine.lastSeenAtMs ? `<span class="badge">Seen ${escapeHtml(formatAgo(machine.lastSeenAtMs))}</span>` : ""}
              </div>
              <div class="row">
                <a class="button-link" href="${machineHref(machine.serial, "recipes")}">Open machine</a>
                <button class="secondary" data-action="forget-machine" data-serial="${escapeHtml(machine.serial)}">Forget</button>
              </div>
            </article>
          `).join("")}
        </section>`
      : `<div class="empty">The bridge is ready, but no coffee machine is saved yet. Go to <a href="#/add"><strong>Add coffee machine</strong></a>, probe a nearby address, and save it with a friendly name.</div>`;

    const stats = `
      <section class="stats-grid">
        <article class="stat-card">
          <h3>${state.machines.length}</h3>
          <div class="muted">saved machines</div>
        </article>
        <article class="stat-card">
          <h3>${state.machines.filter((machine) => machine.online).length}</h3>
          <div class="muted">currently online</div>
        </article>
        <article class="stat-card">
          <h3>${state.status?.supportedDeviceCount ?? 0}</h3>
          <div class="muted">supported matches in latest scan</div>
        </article>
      </section>
    `;

    return sectionShell(
      "Dashboard",
      "Remembered machines stay on the bridge and their online/offline state follows periodic idle BLE scans.",
      `${stats}${machineCards}`
    );
  }

  function renderScanResults() {
    if (!state.scanResults.length) {
      return `<div class="empty">No scan results loaded yet. Start a BLE scan to find nearby coffee machines.</div>`;
    }

    const visibleDevices = state.showCoffeeMachines
      ? state.scanResults.filter(looksLikeCoffeeMachine)
      : state.scanResults;

    if (!visibleDevices.length) {
      return `<div class="empty">No nearby entries currently look like supported coffee machines. Turn off the filter to inspect every BLE advert from the latest scan.</div>`;
    }

    return `
      <div class="scan-list">
        ${visibleDevices.map((device) => `
          <article class="scan-row ${device.likelySupported ? "featured" : ""}">
            <div class="row" style="justify-content:space-between;align-items:flex-start;">
              <div>
                <strong>${escapeHtml(device.name || "Unnamed device")}</strong>
                <div class="muted tiny">${escapeHtml(device.address)}</div>
              </div>
              <div class="badge-row">
                ${device.likelySupported ? '<span class="badge good">likely supported</span>' : ""}
                ${device.advertisedSupportedService ? '<span class="badge good">known protocol service</span>' : ""}
                ${device.connectable ? '<span class="badge">connectable</span>' : '<span class="badge warn">not connectable</span>'}
                <span class="badge">${escapeHtml(String(device.rssi))} dBm</span>
              </div>
            </div>
            <div class="row">
              <button data-action="probe-device" data-address="${escapeHtml(device.address)}">Load details</button>
            </div>
          </article>
        `).join("")}
      </div>
    `;
  }

  function renderProbePreview() {
    if (!state.probedMachine) {
      return "";
    }
    const machine = state.probedMachine;
    const addressType = Number(machine.addressType) === 1 ? "random" : "public";
    return `
      <section class="panel">
        <h2 class="section-title">Probe preview</h2>
        <div class="value-list">
          <div class="value-item"><span>Model</span><span>${escapeHtml(machine.modelName || machine.model || "Unknown")}</span></div>
          <div class="value-item"><span>Serial</span><span>${escapeHtml(machine.serial || "")}</span></div>
          <div class="value-item"><span>Address</span><span>${escapeHtml(machine.address || "")}</span></div>
          <div class="value-item"><span>Address type</span><span>${escapeHtml(addressType)}</span></div>
          <div class="value-item"><span>Family</span><span>${escapeHtml(machine.familyKey || "")}</span></div>
        </div>
        <form data-action="save-probed-machine" style="margin-top:16px;">
          <label class="label" for="probe-alias">Saved name</label>
          <input id="probe-alias" name="alias" type="text" value="${escapeHtml(machine.alias || machine.modelName || machine.serial || "")}" placeholder="Kitchen machine">
          <div class="row" style="margin-top:14px;">
            <button type="submit">Save machine</button>
            <button type="button" class="secondary" data-action="clear-probe">Discard</button>
          </div>
        </form>
      </section>
    `;
  }

  function renderAddPage() {
    return sectionShell(
      "Add coffee machine",
      "Devices that look like supported coffee machines are highlighted first. The bridge probes the address and loads details before it lets you save a remembered machine.",
      `
        <section class="panel">
          <div class="row" style="justify-content:space-between;">
            <div>
              <strong>Nearby devices</strong>
              <div class="muted tiny">Scanning pauses while the bridge is busy with an active machine session.</div>
            </div>
            <div class="row">
              <button data-action="scan-devices">Scan now</button>
              <a class="button-link secondary" href="#/dashboard">Back to dashboard</a>
            </div>
          </div>
          <div class="row" style="margin-top:14px;justify-content:space-between;">
            <label class="row tiny muted" style="gap:8px;">
              <input type="checkbox" data-action="toggle-show-coffee-machines" ${state.showCoffeeMachines ? "checked" : ""}>
              <span>Show coffee machines</span>
            </label>
            <span class="tiny muted">${state.showCoffeeMachines ? state.scanResults.filter(looksLikeCoffeeMachine).length : state.scanResults.length} shown / ${state.scanResults.length} total</span>
          </div>
          <div style="margin-top:14px;">
            ${renderScanResults()}
          </div>
        </section>
        <section class="panel">
          <div class="row" style="justify-content:space-between;align-items:flex-start;">
            <div>
              <strong>Manual coffee machine add</strong>
              <div class="muted tiny">Force-save an offline machine with its BLE address and serial number. Add the model if the serial prefix alone is not enough to decode it.</div>
            </div>
          </div>
          <form data-action="save-manual-machine" style="margin-top:14px;">
            <div class="field-grid">
              <label class="label">Saved name
                <input name="alias" type="text" placeholder="Kitchen machine">
              </label>
              <label class="label">BLE address
                <input name="address" type="text" placeholder="C8:B4:17:D8:A3:8C" spellcheck="false" autocapitalize="characters">
              </label>
              <label class="label">Address type
                <select name="addressType">
                  <option value="public" selected>Public</option>
                  <option value="random">Random</option>
                </select>
              </label>
              <label class="label">Serial number
                <input name="serial" type="text" placeholder="756573071020106-----" spellcheck="false">
              </label>
              <label class="label">Model
                <input name="model" type="text" placeholder="NICR 756" spellcheck="false">
              </label>
            </div>
            <div class="row" style="margin-top:14px;">
              <button type="submit">Save machine</button>
            </div>
          </form>
        </section>
        ${renderProbePreview()}
      `
    );
  }

  function renderMachineHeader(machine, summary, currentSection) {
    const liveStatus = summary?.status || {};
    const percent = Math.max(0, Math.min(100, Number(liveStatus.progress || 0)));
    const canConfirm = machine.online && statusSuggestsHostConfirm(liveStatus);
    return `
      <section class="hero-card machine-hero">
        <div class="row" style="justify-content:space-between;align-items:flex-start;">
          <div>
            <h2>${escapeHtml(machine.alias || machine.modelName || machine.serial)}</h2>
            <div class="machine-subtitle">${escapeHtml(machine.modelName || machine.model || "Unknown model")} · ${escapeHtml(machine.serial)}</div>
          </div>
          <div class="badge-row">
            ${machinePresenceBadge(machine)}
            <span class="badge">${escapeHtml(machine.familyKey || "unknown family")}</span>
          </div>
        </div>
        <div class="value-list">
          <div class="value-item"><span>Live status</span><span>${escapeHtml(liveStatus.summary || "unavailable")}</span></div>
          <div class="value-item"><span>Process</span><span>${escapeHtml(formatStatusCode(liveStatus.processLabel, liveStatus.process))}</span></div>
          <div class="value-item"><span>Machine message</span><span>${escapeHtml(formatStatusCode(liveStatus.messageLabel, liveStatus.message))}</span></div>
          <div class="value-item"><span>Progress</span><span>${percent ? `${percent}%` : "idle"}</span></div>
        </div>
        <div class="progress"><span style="width:${percent}%"></span></div>
        ${canConfirm ? `
          <div class="row" style="margin-top:14px;align-items:center;gap:12px;">
            <button data-action="confirm-machine" data-serial="${escapeHtml(machine.serial)}">${escapeHtml(hostConfirmTitle(liveStatus))}</button>
            <span class="muted tiny">Sends the app-style <code>HY</code> confirmation for machine-driven prompts.</span>
          </div>
        ` : ""}
        <div class="subnav">
          <a class="${currentSection === "recipes" ? "active" : ""}" href="${machineHref(machine.serial, "recipes")}">Coffee recipes</a>
          <a class="${currentSection === "saved" ? "active" : ""}" href="${machineHref(machine.serial, "saved")}">Saved recipes</a>
          <a class="${currentSection === "history" ? "active" : ""}" href="${machineHref(machine.serial, "history")}">Brew history</a>
          <a class="${currentSection === "stats" ? "active" : ""}" href="${machineHref(machine.serial, "stats")}">Statistics</a>
          <a class="${currentSection === "settings" ? "active" : ""}" href="${machineHref(machine.serial, "settings")}">Settings</a>
          <a class="${currentSection === "features" ? "active" : ""}" href="${machineHref(machine.serial, "features")}">Machine features</a>
          <a class="${currentSection === "diagnostics" ? "active" : ""}" href="${machineHref(machine.serial, "diagnostics")}">Diagnostics</a>
        </div>
      </section>
    `;
  }

  function renderRecipesSection(machine, recipesData, savedData) {
    const recipes = recipesData?.recipes || [];
    const savedRecipes = (savedData?.recipes || []).filter((item) => item.ok !== false);
    return `
      <section class="panel">
        <div class="row" style="justify-content:space-between;">
          <h2 class="section-title" style="margin:0;">Coffee recipes</h2>
          <button data-action="refresh-standard-recipes" data-serial="${escapeHtml(machine.serial)}">Refresh recipes</button>
        </div>
        <div class="recipes-grid">
          ${recipes.map((recipe) => `
            <article class="recipe-card">
              <h3>${escapeHtml(recipe.title || recipe.name)}</h3>
              <div class="meta">
                <span>Selector ${escapeHtml(String(recipe.selector))}</span>
                <span>${escapeHtml(recipe.name)}</span>
              </div>
              <div class="row">
                <button data-action="brew-standard" data-serial="${escapeHtml(machine.serial)}" data-selector="${escapeHtml(String(recipe.selector))}">Quick brew</button>
                <a class="button-link secondary" href="${machineStandardRecipeHref(machine.serial, recipe.selector)}">Customize</a>
              </div>
            </article>
          `).join("")}
        </div>
      </section>
      <section class="panel">
        <div class="row" style="justify-content:space-between;">
          <h2 class="section-title" style="margin:0;">Saved recipes</h2>
          <div class="row">
            <button data-action="refresh-saved-recipes" data-serial="${escapeHtml(machine.serial)}">Refresh saved recipes</button>
            <a class="button-link secondary" href="${machineHref(machine.serial, "saved")}">Open all saved recipes</a>
          </div>
        </div>
        ${savedRecipes.length ? `
          <div class="recipe-list">
            ${savedRecipes.map((recipe) => `
              <article class="recipe-card">
                <div class="row" style="justify-content:space-between;">
                  <div>
                    <strong>${escapeHtml(recipe.name || `Slot ${recipe.slot}`)}</strong>
                    <div class="muted tiny">${escapeHtml(recipe.typeName || "custom")} · slot ${escapeHtml(String(recipe.slot))}</div>
                  </div>
                  ${recipe.enabled ? '<span class="badge good">enabled</span>' : '<span class="badge warn">disabled</span>'}
                </div>
                <div class="row">
                  <a class="button-link ghost" href="${machineRecipeHref(machine.serial, recipe.slot)}">View details</a>
                </div>
              </article>
            `).join("")}
          </div>
        ` : '<div class="empty">No saved recipes were returned for this machine.</div>'}
      </section>
    `;
  }

  function renderSavedRecipesSection(machine, data) {
    const items = (data?.recipes || []).filter((item) => item.ok !== false);
    if (!items.length) {
      return `<section class="panel"><div class="empty">No saved custom recipes were returned by the machine.</div></section>`;
    }
    return `
      <section class="panel">
        <div class="row" style="justify-content:space-between;">
          <h2 class="section-title" style="margin:0;">Saved recipes</h2>
          <button data-action="refresh-saved-recipes" data-serial="${escapeHtml(machine.serial)}">Refresh saved recipes</button>
        </div>
        <div class="recipe-list">
          ${items.map((recipe) => `
            <article class="recipe-card">
              <div class="row" style="justify-content:space-between;">
                <div>
                  <h3>${escapeHtml(recipe.name || `Slot ${recipe.slot}`)}</h3>
                  <div class="meta">
                    <span>${escapeHtml(recipe.typeName || "custom")}</span>
                    <span>Slot ${escapeHtml(String(recipe.slot))}</span>
                  </div>
                </div>
                ${recipe.enabled ? '<span class="badge good">enabled</span>' : '<span class="badge warn">disabled</span>'}
              </div>
              <div class="badge-row">
                ${recipe.nameRegister ? `<span class="badge">name @ ${escapeHtml(String(recipe.nameRegister))}</span>` : ""}
                ${recipe.typeRegister ? `<span class="badge">type @ ${escapeHtml(String(recipe.typeRegister))}</span>` : ""}
              </div>
              <div class="row">
                <a class="button-link" href="${machineRecipeHref(machine.serial, recipe.slot)}">View details</a>
              </div>
            </article>
          `).join("")}
        </div>
      </section>
    `;
  }

  function recipeEditableFields(recipe, options = {}) {
    const fields = [];
    const includeName = options.includeName !== false;
    const includeIcon = options.includeIcon !== false;
    const maxStrengthBeans = Math.max(1, Math.min(5, Number(recipe.maxStrengthBeans || 5)));
    const availableAromaOptions = PROFILE_OPTIONS.filter((option) => option.value <= Number(recipe.maxProfileCode ?? 4));
    const pushSelect = (name, title, options, current) => {
      if (recipe[name] === undefined) {
        return;
      }
      fields.push(`
        <label class="label">${escapeHtml(title)}
          <select name="${escapeHtml(name)}">
            ${options.map((option) => `
              <option value="${escapeHtml(String(option.value))}" ${Number(current) === Number(option.value) ? "selected" : ""}>${escapeHtml(option.label)}</option>
            `).join("")}
          </select>
        </label>
      `);
    };
    const pushAmount = (name, title, current) => {
      if (recipe[name] === undefined) {
        return;
      }
      fields.push(`
        <label class="label">${escapeHtml(title)}
          <input type="number" step="1" min="0" name="${escapeHtml(name)}" value="${escapeHtml(String(current))}">
        </label>
      `);
    };

    if (includeName) {
      fields.push(`
        <label class="label">Name
          <input type="text" name="name" value="${escapeHtml(recipe.name || "")}">
        </label>
      `);
    }

    if (includeIcon && recipe.icon !== undefined) {
      fields.push(`
        <label class="label">Icon
          <input type="number" step="1" min="0" name="icon" value="${escapeHtml(String(recipe.icon))}">
        </label>
      `);
    }

    if (recipe.strength !== undefined) {
      fields.push(`
        <label class="label">Strength
          <select name="strengthBeans">
            ${Array.from({ length: maxStrengthBeans }, (_, index) => index + 1).map((count) => `
              <option value="${count}" ${Number(recipe.strengthBeans || (recipe.strength + 1)) === count ? "selected" : ""}>${count} beans</option>
            `).join("")}
          </select>
        </label>
      `);
    }

    pushSelect("aroma", "Aroma", availableAromaOptions, recipe.aroma);
    pushSelect("temperature", "Temperature", TEMPERATURE_OPTIONS, recipe.temperature);
    pushSelect("coffeeTemperature", "Coffee temperature", TEMPERATURE_OPTIONS, recipe.coffeeTemperature);
    pushSelect("waterTemperature", "Water temperature", TEMPERATURE_OPTIONS, recipe.waterTemperature);
    pushSelect("milkTemperature", "Milk temperature", TEMPERATURE_OPTIONS, recipe.milkTemperature);
    pushSelect("milkFoamTemperature", "Milk foam temperature", TEMPERATURE_OPTIONS, recipe.milkFoamTemperature);
    pushSelect("overallTemperature", "Overall temperature", TEMPERATURE_OPTIONS, recipe.overallTemperature);
    pushSelect("twoCups", "Two cups", ON_OFF_OPTIONS, recipe.twoCups);

    pushAmount("coffeeAmountMl", "Coffee amount (ml)", recipe.coffeeAmountMl);
    pushAmount("waterAmountMl", "Water amount (ml)", recipe.waterAmountMl);
    pushAmount("milkAmountMl", "Milk amount (ml)", recipe.milkAmountMl);
    pushAmount("milkFoamAmountMl", "Milk foam amount (ml)", recipe.milkFoamAmountMl);

    return fields.join("");
  }

  function renderRecipeDetail(machine, data, slot) {
    const recipe = data?.recipe;
    if (!recipe) {
      return `<section class="panel"><div class="empty">Recipe slot ${escapeHtml(String(slot))} could not be loaded.</div></section>`;
    }
    const sourceLabel = data?.source === "cache" ? "flash cache" : "live machine";

    return `
      <section class="panel">
        <div class="row" style="justify-content:space-between;">
          <div>
            <h2 class="section-title" style="margin:0;">${escapeHtml(recipe.name || `Recipe slot ${slot}`)}</h2>
            <div class="muted">Saved custom recipe details for slot ${escapeHtml(String(slot))}</div>
          </div>
          <div class="row">
            <button data-action="refresh-saved-recipe" data-serial="${escapeHtml(machine.serial)}" data-slot="${escapeHtml(String(slot))}">Refresh from machine</button>
            <a class="button-link secondary" href="${machineHref(machine.serial, "saved")}">Back to saved recipes</a>
          </div>
        </div>
        <div class="grid two" style="margin-top:16px;">
          <div class="panel" style="margin:0;padding:18px;">
            <div class="value-list">
              <div class="value-item"><span>Name</span><span>${escapeHtml(recipe.name || "")}</span></div>
              <div class="value-item"><span>Source</span><span>${escapeHtml(sourceLabel)}</span></div>
              <div class="value-item"><span>Icon</span><span>${escapeHtml(recipe.icon ?? "-")}</span></div>
              <div class="value-item"><span>Strength</span><span>${escapeHtml(recipe.strengthBeans ? `${recipe.strengthBeans} beans` : "-")}</span></div>
              <div class="value-item"><span>Temperature</span><span>${escapeHtml(recipe.temperatureLabel || recipe.coffeeTemperatureLabel || "-")}</span></div>
              <div class="value-item"><span>Size</span><span>${escapeHtml(recipe.coffeeAmountMl ?? recipe.waterAmountMl ?? "-")}${recipe.coffeeAmountMl !== undefined || recipe.waterAmountMl !== undefined ? " ml" : ""}</span></div>
              <div class="value-item"><span>Type</span><span>${escapeHtml(recipe.typeName || "-")}</span></div>
            </div>
          </div>
          <div class="panel" style="margin:0;padding:18px;">
            <h3 style="margin-top:0;">Edit recipe</h3>
            <form data-action="save-recipe" data-serial="${escapeHtml(machine.serial)}" data-slot="${escapeHtml(String(slot))}">
              <div class="field-grid">
                ${recipeEditableFields(recipe)}
              </div>
              <div class="row" style="margin-top:14px;">
                <button type="submit">Save recipe</button>
              </div>
            </form>
          </div>
        </div>
        <details style="margin-top:16px;">
          <summary>Raw recipe JSON</summary>
          <pre>${escapeHtml(pretty(recipe))}</pre>
        </details>
      </section>
    `;
  }

  function renderStandardRecipeDetail(machine, data, selector) {
    const recipe = data?.recipe;
    if (!recipe) {
      return `<section class="panel"><div class="empty">Standard recipe ${escapeHtml(String(selector))} could not be loaded.</div></section>`;
    }
    const sourceLabel = data?.source === "cache" ? "flash cache" : "live machine";

    return `
      <section class="panel">
        <div class="row" style="justify-content:space-between;">
          <div>
            <h2 class="section-title" style="margin:0;">${escapeHtml(recipe.title || recipe.name || `Recipe ${selector}`)}</h2>
            <div class="muted">Standard recipe values are cached on the bridge. Brew overrides are temporary and do not rewrite the saved standard recipe.</div>
          </div>
          <div class="row">
            <button data-action="refresh-standard-recipe" data-serial="${escapeHtml(machine.serial)}" data-selector="${escapeHtml(String(selector))}">Refresh from machine</button>
            <a class="button-link secondary" href="${machineHref(machine.serial, "recipes")}">Back to coffee recipes</a>
          </div>
        </div>
        <div class="grid two" style="margin-top:16px;">
          <div class="panel" style="margin:0;padding:18px;">
            <div class="value-list">
              <div class="value-item"><span>Name</span><span>${escapeHtml(recipe.title || recipe.name || "-")}</span></div>
              <div class="value-item"><span>Selector</span><span>${escapeHtml(String(recipe.selector ?? selector))}</span></div>
              <div class="value-item"><span>Source</span><span>${escapeHtml(sourceLabel)}</span></div>
              <div class="value-item"><span>Strength</span><span>${escapeHtml(recipe.strengthBeans ? `${recipe.strengthBeans} beans` : "-")}</span></div>
              <div class="value-item"><span>Aroma</span><span>${escapeHtml(recipe.aromaLabel || "-")}</span></div>
              <div class="value-item"><span>Temperature</span><span>${escapeHtml(recipe.temperatureLabel || recipe.coffeeTemperatureLabel || "-")}</span></div>
              <div class="value-item"><span>Size</span><span>${escapeHtml(recipe.coffeeAmountMl ?? recipe.waterAmountMl ?? "-")}${recipe.coffeeAmountMl !== undefined || recipe.waterAmountMl !== undefined ? " ml" : ""}</span></div>
            </div>
          </div>
          <div class="panel" style="margin:0;padding:18px;">
            <h3 style="margin-top:0;">Temporary brew values</h3>
            <form data-action="brew-standard-custom" data-serial="${escapeHtml(machine.serial)}" data-selector="${escapeHtml(String(selector))}">
              <div class="field-grid">
                ${recipeEditableFields(recipe, { includeName: false, includeIcon: false })}
              </div>
              <div class="row" style="margin-top:14px;">
                <button type="submit">Make coffee with these values</button>
              </div>
            </form>
          </div>
        </div>
        <details style="margin-top:16px;">
          <summary>Raw recipe JSON</summary>
          <pre>${escapeHtml(pretty(recipe))}</pre>
        </details>
      </section>
    `;
  }

  function renderStatsSection(machine, data, history) {
    if (!data?.supported) {
      return `<section class="panel"><div class="empty">Statistics are not currently implemented for this machine family in the bridge firmware.</div></section>`;
    }

    const values = Object.values(data.values || {});
    const beverages = values.filter((item) => item.section === "beverages");
    const maintenance = values.filter((item) => item.section === "maintenance");
    const metricMeta = data?.values || {};
    const historyEntries = history?.entries || [];
    const renderMetricGroup = (title, items) => `
      <section class="panel">
        <h2 class="section-title">${escapeHtml(title)}</h2>
        ${items.length ? `
          <div class="value-list">
            ${items.map((item) => `
              <div class="value-item">
                <span>${escapeHtml(item.title)}</span>
                <span>${escapeHtml(String(item.rawValue))}${item.unit && item.unit !== "count" && item.unit !== "flag" ? ` ${escapeHtml(item.unit)}` : ""}</span>
              </div>
            `).join("")}
          </div>
        ` : '<div class="empty">No values were returned for this section.</div>'}
      </section>
    `;

    const renderStatsHistoryMetric = (entry, key) => {
      const item = metricMeta[key] || {};
      const title = item.title || key.replace(/_/g, " ");
      const unit = item.unit && item.unit !== "count" && item.unit !== "flag" ? ` ${item.unit}` : "";
      const value = entry?.values?.[key];
      const delta = entry?.delta?.[key];
      const deltaText = hasNumericValue(delta) ? `${Number(delta) >= 0 ? "+" : ""}${Number(delta)}` : "";
      return `
        <div class="value-item">
          <span>${escapeHtml(title)}</span>
          <span>${escapeHtml(String(value ?? "-"))}${escapeHtml(unit)}${deltaText ? ` (${escapeHtml(deltaText)})` : ""}</span>
        </div>
      `;
    };

    const renderStatsHistory = () => {
      const total = Math.max(0, Number(history?.count || 0));
      const offset = Math.max(0, Number(history?.offset || 0));
      const returned = Math.max(0, Number(history?.returned || historyEntries.length));
      const rangeStart = returned ? offset + 1 : 0;
      const rangeEnd = returned ? offset + returned : 0;
      return `
        <section class="panel">
          <div class="row" style="justify-content:space-between;align-items:flex-start;">
            <div>
              <h2 class="section-title" style="margin:0;">Counter history</h2>
              <div class="muted">The bridge stores a snapshot when live counters change. This captures local machine use even when a coffee was started from the front panel.</div>
            </div>
            <div class="row">
              <span class="badge">${escapeHtml(String(history?.count ?? 0))} stored</span>
              <span class="badge">${escapeHtml(String(history?.fileBytes ?? 0))} / ${escapeHtml(String(history?.maxBytes ?? 0))} bytes</span>
              <button class="secondary" data-action="refresh-machine-stats" data-serial="${escapeHtml(machine.serial)}">Refresh stats</button>
              <button class="warn" data-action="clear-stats-history" data-serial="${escapeHtml(machine.serial)}">Clear history</button>
            </div>
          </div>
          <div class="row" style="margin-top:14px;justify-content:space-between;">
            <div class="muted">
              ${returned ? `Showing newest ${escapeHtml(String(rangeStart))}-${escapeHtml(String(rangeEnd))} of ${escapeHtml(String(total))}.` : "No counter snapshots stored yet."}
            </div>
            <div class="row">
              <button class="secondary" data-action="stats-history-prev-page" data-serial="${escapeHtml(machine.serial)}" data-offset="${escapeHtml(String(history?.prevOffset ?? 0))}" ${history?.hasNewer ? "" : "disabled"}>Newer</button>
              <button class="secondary" data-action="stats-history-next-page" data-serial="${escapeHtml(machine.serial)}" data-offset="${escapeHtml(String(history?.nextOffset ?? 0))}" ${history?.hasOlder ? "" : "disabled"}>Older</button>
            </div>
          </div>
          ${historyEntries.length ? `
            <div class="recipe-list" style="margin-top:16px;">
              ${historyEntries.map((entry) => {
                const changedKeys = Array.isArray(entry?.changedKeys) && entry.changedKeys.length
                  ? entry.changedKeys
                  : (entry?.baseline ? ["total_beverages", "beverages_via_app"].filter((key) => entry?.values?.[key] !== undefined) : Object.keys(entry?.delta || {}));
                const summary = entry?.baseline
                  ? "Initial counter baseline"
                  : `${Number(entry?.changedCount || changedKeys.length || 0)} metrics changed${hasNumericValue(entry?.totalDelta) ? ` · total beverages ${Number(entry.totalDelta) >= 0 ? "+" : ""}${Number(entry.totalDelta)}` : ""}`;
                return `
                  <article class="recipe-card">
                    <div class="row" style="justify-content:space-between;align-items:flex-start;">
                      <div>
                        <h3>${escapeHtml(summary)}</h3>
                        <div class="meta">
                          <span>${escapeHtml(formatHistoryTimestamp(entry))}</span>
                          <span>${escapeHtml(entry.source || "stats")}</span>
                        </div>
                      </div>
                      <div class="badge-row">
                        ${entry?.baseline ? '<span class="badge">baseline</span>' : ""}
                        ${hasNumericValue(entry?.totalDelta) ? `<span class="badge">${escapeHtml(`${Number(entry.totalDelta) >= 0 ? "+" : ""}${Number(entry.totalDelta)} total`)}</span>` : ""}
                      </div>
                    </div>
                    <div class="value-list">
                      ${changedKeys.length ? changedKeys.map((key) => renderStatsHistoryMetric(entry, key)).join("") : '<div class="value-item"><span>Metrics</span><span>No changes captured</span></div>'}
                    </div>
                    <details>
                      <summary>Raw counter snapshot</summary>
                      <pre>${escapeHtml(pretty(entry))}</pre>
                    </details>
                  </article>
                `;
              }).join("")}
            </div>
          ` : '<div class="empty" style="margin-top:16px;">No counter snapshots stored yet.</div>'}
        </section>
      `;
    };

    return `
      ${renderMetricGroup("Ordered beverages", beverages)}
      ${renderMetricGroup("Maintenance", maintenance)}
      ${renderStatsHistory()}
      <section class="panel">
        <h2 class="section-title">Serial and details</h2>
        <div class="value-list">
          <div class="value-item"><span>Manufacturer</span><span>${escapeHtml(data.details?.manufacturer || "-")}</span></div>
          <div class="value-item"><span>Model</span><span>${escapeHtml(data.details?.model || "-")}</span></div>
          <div class="value-item"><span>Serial</span><span>${escapeHtml(data.details?.serial || "-")}</span></div>
          <div class="value-item"><span>Firmware</span><span>${escapeHtml(data.details?.firmwareRevision || "-")}</span></div>
        </div>
      </section>
      <details>
        <summary>Raw statistics JSON</summary>
        <pre>${escapeHtml(pretty(data))}</pre>
      </details>
    `;
  }

  function formatHistoryTimestamp(entry) {
    const unix = Number(entry?.timeUnix || 0);
    if (unix > 0) {
      try {
        return new Date(unix * 1000).toLocaleString();
      } catch (error) {
        return String(entry?.timeIsoUtc || "");
      }
    }
    const loggedAtMs = Number(entry?.loggedAtMs || 0);
    if (loggedAtMs > 0) {
      return `uptime ${Math.round(loggedAtMs / 1000)}s`;
    }
    return "unknown";
  }

  function brewHistorySummary(entry) {
    const recipe = entry?.recipe || {};
    const parts = [];
    if (recipe.strengthBeans !== undefined) {
      parts.push(`${recipe.strengthBeans} beans`);
    }
    if (recipe.aromaLabel) {
      parts.push(recipe.aromaLabel);
    }
    if (recipe.temperatureLabel || recipe.coffeeTemperatureLabel || recipe.overallTemperatureLabel) {
      parts.push(recipe.temperatureLabel || recipe.coffeeTemperatureLabel || recipe.overallTemperatureLabel);
    }
    const sizeMl = recipe.coffeeAmountMl ?? recipe.waterAmountMl;
    if (sizeMl !== undefined) {
      parts.push(`${sizeMl} ml`);
    }
    return parts.join(" · ");
  }

  function hasNumericValue(value) {
    return value !== null && value !== undefined && Number.isFinite(Number(value));
  }

  function buildHistoryReplayPayload(entry) {
    const recipe = entry?.recipe || {};
    const selector = Number(entry?.selector ?? recipe.selector);
    if (!Number.isFinite(selector) || selector < 0) {
      throw new Error("This history entry cannot be replayed because it has no recipe selector.");
    }

    const payload = { selector, source: "ui-history-replay" };
    const numericFields = [
      "strength",
      "strengthBeans",
      "aroma",
      "temperature",
      "coffeeTemperature",
      "waterTemperature",
      "milkTemperature",
      "milkFoamTemperature",
      "overallTemperature",
      "preparation",
      "twoCups",
      "coffeeAmountMl",
      "waterAmountMl",
      "milkAmountMl",
      "milkFoamAmountMl"
    ];
    numericFields.forEach((field) => {
      if (hasNumericValue(recipe[field])) {
        payload[field] = Number(recipe[field]);
      }
    });
    if (entry?.label) {
      payload.label = String(entry.label);
    }
    return payload;
  }

  function historyReplaySummaryLines(entry) {
    const recipe = entry?.recipe || {};
    const lines = [];
    if (hasNumericValue(recipe.strengthBeans)) {
      lines.push(`Beans: ${Number(recipe.strengthBeans)}`);
    }
    if (recipe.aromaLabel) {
      lines.push(`Aroma: ${recipe.aromaLabel}`);
    } else if (hasNumericValue(recipe.aroma)) {
      lines.push(`Aroma code: ${Number(recipe.aroma)}`);
    }
    if (recipe.temperatureLabel) {
      lines.push(`Temperature: ${recipe.temperatureLabel}`);
    }
    if (recipe.coffeeTemperatureLabel) {
      lines.push(`Coffee temp: ${recipe.coffeeTemperatureLabel}`);
    }
    if (recipe.waterTemperatureLabel) {
      lines.push(`Water temp: ${recipe.waterTemperatureLabel}`);
    }
    if (recipe.milkTemperatureLabel) {
      lines.push(`Milk temp: ${recipe.milkTemperatureLabel}`);
    }
    if (recipe.milkFoamTemperatureLabel) {
      lines.push(`Foam temp: ${recipe.milkFoamTemperatureLabel}`);
    }
    if (recipe.overallTemperatureLabel) {
      lines.push(`Overall temp: ${recipe.overallTemperatureLabel}`);
    }
    if (hasNumericValue(recipe.twoCups)) {
      lines.push(`Cup mode: ${Number(recipe.twoCups) ? "two cups" : "one cup"}`);
    }
    if (hasNumericValue(recipe.preparation)) {
      lines.push(`Preparation: ${Number(recipe.preparation)}`);
    }
    if (hasNumericValue(recipe.coffeeAmountMl)) {
      lines.push(`Coffee: ${Number(recipe.coffeeAmountMl)} ml`);
    }
    if (hasNumericValue(recipe.waterAmountMl) && Number(recipe.waterAmountMl) > 0) {
      lines.push(`Water: ${Number(recipe.waterAmountMl)} ml`);
    }
    if (hasNumericValue(recipe.milkAmountMl) && Number(recipe.milkAmountMl) > 0) {
      lines.push(`Milk: ${Number(recipe.milkAmountMl)} ml`);
    }
    if (hasNumericValue(recipe.milkFoamAmountMl) && Number(recipe.milkFoamAmountMl) > 0) {
      lines.push(`Milk foam: ${Number(recipe.milkFoamAmountMl)} ml`);
    }
    return lines;
  }

  function renderHistoryReplayConfirmBody(entry) {
    const title = String(entry?.label || entry?.recipeTitle || entry?.recipeName || "Logged brew");
    const drink = String(entry?.recipeTitle || entry?.recipeName || entry?.recipe?.title || entry?.recipe?.name || title);
    const summaryLines = historyReplaySummaryLines(entry);
    return `
      <p class="muted">This replays the saved brew values from the selected history entry back to the machine.</p>
      <div class="value-list">
        <div class="value-item"><span>Drink</span><span>${escapeHtml(drink)}</span></div>
        ${title && title !== drink ? `<div class="value-item"><span>Label</span><span>${escapeHtml(title)}</span></div>` : ""}
        <div class="value-item"><span>Original brew</span><span>${escapeHtml(formatHistoryTimestamp(entry))}</span></div>
        ${entry?.recipeFingerprint ? `<div class="value-item"><span>Fingerprint</span><span>${escapeHtml(entry.recipeFingerprint)}</span></div>` : ""}
      </div>
      ${summaryLines.length ? `
        <div class="stack">
          <div class="muted tiny">Saved brew values</div>
          <div class="badge-row">
            ${summaryLines.map((line) => `<span class="badge">${escapeHtml(line)}</span>`).join("")}
          </div>
        </div>
      ` : ""}
    `;
  }

  function renderHistorySection(machine, data) {
    const entries = data?.entries || [];
    const total = Math.max(0, Number(data?.count || 0));
    const offset = Math.max(0, Number(data?.offset || 0));
    const returned = Math.max(0, Number(data?.returned || entries.length));
    const rangeStart = returned ? offset + 1 : 0;
    const rangeEnd = returned ? offset + returned : 0;
    return `
      <section class="panel">
        <div class="row" style="justify-content:space-between;">
          <div>
            <h2 class="section-title" style="margin:0;">Brew history</h2>
            <div class="muted">The bridge stores a bounded per-machine history in flash and keeps only the newest entries.</div>
          </div>
          <div class="row">
            <span class="badge">${escapeHtml(String(data?.count ?? 0))} stored</span>
            <span class="badge">${escapeHtml(String(data?.fileBytes ?? 0))} / ${escapeHtml(String(data?.maxBytes ?? 0))} bytes</span>
            <button class="secondary" data-action="refresh-history" data-serial="${escapeHtml(machine.serial)}">Refresh history</button>
            <button class="warn" data-action="clear-history" data-serial="${escapeHtml(machine.serial)}">Clear history</button>
          </div>
        </div>
        <div class="row" style="margin-top:14px;justify-content:space-between;">
          <div class="muted">
            ${returned ? `Showing newest ${escapeHtml(String(rangeStart))}-${escapeHtml(String(rangeEnd))} of ${escapeHtml(String(total))}.` : "No stored brews yet."}
          </div>
          <div class="row">
            <button class="secondary" data-action="history-prev-page" data-serial="${escapeHtml(machine.serial)}" data-offset="${escapeHtml(String(data?.prevOffset ?? 0))}" ${data?.hasNewer ? "" : "disabled"}>Newer</button>
            <button class="secondary" data-action="history-next-page" data-serial="${escapeHtml(machine.serial)}" data-offset="${escapeHtml(String(data?.nextOffset ?? 0))}" ${data?.hasOlder ? "" : "disabled"}>Older</button>
          </div>
        </div>
        ${entries.length ? `
          <div class="recipe-list" style="margin-top:16px;">
            ${entries.map((entry, index) => {
              const canReplay = Number.isFinite(Number(entry?.selector ?? entry?.recipe?.selector)) &&
                Number(entry?.selector ?? entry?.recipe?.selector) >= 0;
              return `
              <article class="recipe-card">
                <div class="row" style="justify-content:space-between;align-items:flex-start;">
                  <div>
                    <h3>${escapeHtml(entry.label || entry.recipeTitle || entry.recipeName || "Logged brew")}</h3>
                    <div class="meta">
                      <span>${escapeHtml(formatHistoryTimestamp(entry))}</span>
                      <span>${escapeHtml(entry.source || "api")}${entry.actor ? ` · ${escapeHtml(entry.actor)}` : ""}</span>
                    </div>
                  </div>
                  <div class="badge-row">
                    <span class="badge">${escapeHtml(entry.recipeFingerprint || "")}</span>
                    <span class="badge">${escapeHtml(entry.statusSummary || entry.result || "accepted")}</span>
                  </div>
                </div>
                <div class="value-list">
                  <div class="value-item"><span>Drink</span><span>${escapeHtml(entry.recipeTitle || entry.recipeName || "-")}</span></div>
                  <div class="value-item"><span>Summary</span><span>${escapeHtml(brewHistorySummary(entry) || "-")}</span></div>
                  <div class="value-item"><span>Machine message</span><span>${escapeHtml(formatStatusCode(entry.messageLabel, entry.message))}</span></div>
                </div>
                <div class="row">
                  <button class="secondary" data-action="brew-history-again" data-serial="${escapeHtml(machine.serial)}" data-index="${escapeHtml(String(index))}" ${canReplay ? "" : "disabled"}>Brew again</button>
                </div>
                ${entry.note ? `<div class="muted">${escapeHtml(entry.note)}</div>` : ""}
                <details>
                  <summary>Raw history entry</summary>
                  <pre>${escapeHtml(pretty(entry))}</pre>
                </details>
              </article>
            `;
            }).join("")}
          </div>
        ` : '<div class="empty" style="margin-top:16px;">No brews have been logged for this machine yet.</div>'}
      </section>
    `;
  }

  function settingSelectHtml(key, currentLabel) {
    const values = SETTING_OPTIONS[key] || [];
    return `
      <select name="value" ${values.length ? "" : "disabled"}>
        ${values.map((value) => `<option value="${escapeHtml(value)}" ${String(currentLabel).toLowerCase() === String(value).toLowerCase() ? "selected" : ""}>${escapeHtml(value)}</option>`).join("")}
      </select>
    `;
  }

  function renderSettingsSection(machine, data) {
    const values = Object.entries(data?.values || {});
    if (!values.length) {
      return `<section class="panel"><div class="empty">No supported settings were returned for this machine.</div></section>`;
    }

    return `
      <section class="panel">
        <h2 class="section-title">Basic settings</h2>
        <div class="stack">
          ${values.map(([key, item]) => `
            <form class="setting-row" data-action="save-setting" data-serial="${escapeHtml(machine.serial)}" data-key="${escapeHtml(key)}">
              <div class="row" style="justify-content:space-between;align-items:flex-start;">
                <div>
                  <strong>${escapeHtml(item.title || key)}</strong>
                  <div class="muted tiny">Register ${escapeHtml(String(item.registerId || ""))}</div>
                </div>
                <span class="badge">${escapeHtml(item.valueLabel || item.valueCodeHex || "")}</span>
              </div>
              <div class="field-grid">
                <label class="label">Value
                  ${settingSelectHtml(key, item.valueLabel || "")}
                </label>
              </div>
              <div class="row">
                <button type="submit">Save</button>
              </div>
            </form>
          `).join("")}
        </div>
      </section>
      <section class="panel">
        <h2 class="section-title">Factory reset actions</h2>
        <div class="stack">
          <form class="setting-row" data-action="factory-reset" data-serial="${escapeHtml(machine.serial)}" data-reset-action="factory_reset_settings">
            <div>
              <strong>Factory settings: Settings</strong>
              <div class="muted tiny">Recovered app mapping: <code>HE 00 32</code> with the remaining 16 bytes set to zero.</div>
            </div>
            <div class="row">
              <button type="submit" class="warn">Reset settings</button>
            </div>
          </form>
          <form class="setting-row" data-action="factory-reset" data-serial="${escapeHtml(machine.serial)}" data-reset-action="factory_reset_recipes">
            <div>
              <strong>Factory settings: Recipes</strong>
              <div class="muted tiny">Recovered app mapping: <code>HE 00 33</code> with the remaining 16 bytes set to zero.</div>
            </div>
            <div class="row">
              <button type="submit" class="warn">Reset recipes</button>
            </div>
          </form>
        </div>
      </section>
      <details>
        <summary>Raw settings JSON</summary>
        <pre>${escapeHtml(pretty(data))}</pre>
      </details>
    `;
  }

  function renderMachineFeaturesSection(machine, data) {
    const features = data?.features || {};
    const knownFlags = Array.isArray(features?.knownFlags) ? features.knownFlags : [];
    const bytes = Array.isArray(features?.bytes) ? features.bytes : [];
    const unknownBits = Array.isArray(features?.unknownNonZeroBits) ? features.unknownNonZeroBits : [];

    return `
      <section class="panel">
        <div class="row" style="justify-content:space-between;align-items:flex-start;">
          <div>
            <h2 class="section-title" style="margin:0;">Machine features</h2>
            <div class="muted">This page reads the APK-backed <code>HI</code> capability payload. Only flags explicitly named in the Android app are decoded semantically; remaining non-zero bits stay raw.</div>
          </div>
          <div class="row">
            <button class="secondary" data-action="refresh-machine-features" data-serial="${escapeHtml(machine.serial)}">Refresh features</button>
          </div>
        </div>
        <div class="value-list" style="margin-top:16px;">
          <div class="value-item"><span>APK source</span><span>${escapeHtml(features.source || "de.nivona.mobileapp 3.8.6")}</span></div>
          <div class="value-item"><span>Raw payload</span><span><code>${escapeHtml(features.rawHex || "-")}</code></span></div>
          <div class="value-item"><span>Payload size</span><span>${escapeHtml(String(features.payloadSize || bytes.length || 0))} bytes</span></div>
          <div class="value-item"><span>APK-known flags enabled</span><span>${escapeHtml(String(features.knownEnabledCount || 0))} / ${escapeHtml(String(features.knownFlagCount || knownFlags.length))}</span></div>
          <div class="value-item"><span>Unknown non-zero bits</span><span>${escapeHtml(String(features.unknownNonZeroBitCount || unknownBits.length))}</span></div>
        </div>
      </section>
      <section class="panel">
        <h2 class="section-title">APK-known flags</h2>
        ${knownFlags.length ? `
          <div class="stack">
            ${knownFlags.map((flag) => `
              <article class="recipe-card">
                <div class="row" style="justify-content:space-between;align-items:flex-start;">
                  <div>
                    <strong>${escapeHtml(flag.title || flag.key || "feature")}</strong>
                    <div class="muted tiny">${escapeHtml(flag.key || "")} · byte ${escapeHtml(String(flag.byteIndex ?? 0))} · mask ${escapeHtml(flag.maskHex || formatByteHex(flag.mask))}</div>
                  </div>
                  ${flag.enabled ? '<span class="badge good">enabled</span>' : '<span class="badge warn">off</span>'}
                </div>
                <div class="muted tiny">Source: ${escapeHtml(flag.source || "apk-3.8.6")}</div>
              </article>
            `).join("")}
          </div>
        ` : '<div class="empty">The current bridge build does not expose any named HI flags yet.</div>'}
      </section>
      <section class="panel">
        <h2 class="section-title">Raw HI bytes</h2>
        ${bytes.length ? `
          <div class="value-list">
            ${bytes.map((value, index) => `
              <div class="value-item">
                <span>Byte ${escapeHtml(String(index))}</span>
                <span><code>${escapeHtml(formatByteHex(value))}</code> · ${escapeHtml(formatByteBits(value))}</span>
              </div>
            `).join("")}
          </div>
        ` : '<div class="empty">No raw HI payload bytes were returned.</div>'}
      </section>
      <section class="panel">
        <h2 class="section-title">Unknown non-zero bits</h2>
        ${unknownBits.length ? `
          <div class="stack">
            ${unknownBits.map((bit) => `
              <article class="recipe-card">
                <div class="row" style="justify-content:space-between;align-items:flex-start;">
                  <div>
                    <strong>Byte ${escapeHtml(String(bit.byteIndex ?? 0))} · ${escapeHtml(bit.maskHex || formatByteHex(bit.mask))}</strong>
                    <div class="muted tiny">Observed byte value ${escapeHtml(bit.valueHex || formatByteHex(bit.value))} while no APK-defined meaning is known yet.</div>
                  </div>
                  <span class="badge">${escapeHtml(formatByteBits(bit.value))}</span>
                </div>
              </article>
            `).join("")}
          </div>
        ` : '<div class="empty">No unknown non-zero bits are set in the current HI response.</div>'}
      </section>
      <details>
        <summary>Raw machine features JSON</summary>
        <pre>${escapeHtml(pretty(data))}</pre>
      </details>
    `;
  }

  function diagnosticsDefault(machine) {
    return {
      address: machine.address,
      waitMs: 3000,
      notificationMode: "notify",
      encrypt: true,
      reconnectAfterPair: true,
      pair: true
    };
  }

  function renderDiagnosticsSection(machine, summary) {
    const output = state.diagnostics.output ? escapeHtml(pretty(state.diagnostics.output)) : "{}";
    const logs = state.diagnostics.logs ? escapeHtml(pretty(state.diagnostics.logs)) : "No logs loaded.";
    const defaults = diagnosticsDefault(machine);
    const protocolSession = summary?.protocolSession || {};
    const liveStatus = summary?.status || {};
    const sessionAge = protocolSession.setAtMs ? formatAgo(protocolSession.setAtMs) : "";
    return `
      <section class="panel">
        <h2 class="section-title">Diagnostics</h2>
        <p class="muted">This page keeps the raw bridge packet tools, but scopes every request to the saved machine currently opened in the dashboard.</p>
      </section>

      <section class="grid two">
        <article class="diag-block">
          <h3>Protocol session key</h3>
          <div class="value-list" style="margin-bottom:16px;">
            <div class="value-item"><span>Current session</span><span>${protocolSession.hasSession ? escapeHtml(protocolSession.sessionHex) : "none"}</span></div>
            <div class="value-item"><span>Source</span><span>${escapeHtml(protocolSession.source || "-")}</span></div>
            <div class="value-item"><span>Updated</span><span>${escapeHtml(sessionAge || "never")}</span></div>
          </div>
          <form data-action="diag-session-set">
            <input type="hidden" name="serial" value="${escapeHtml(machine.serial)}">
            <input type="hidden" name="address" value="${escapeHtml(machine.address)}">
            <input type="hidden" name="addressType" value="${escapeHtml(String(machine.addressType ?? 0))}">
            <label class="label">Session hex
              <input type="text" name="sessionHex" placeholder="1234">
            </label>
            <label class="label">Source
              <input type="text" name="source" value="machine-diagnostics">
            </label>
            <div class="row">
              <button type="submit">Set session</button>
              <button type="button" class="secondary" data-action="diag-session-clear" data-serial="${escapeHtml(machine.serial)}" data-address="${escapeHtml(machine.address)}" data-address-type="${escapeHtml(String(machine.addressType ?? 0))}">Clear</button>
            </div>
          </form>
        </article>

        <article class="diag-block">
          <h3>Workflow prompt confirmation</h3>
          <p class="muted">Use when the machine is waiting for host confirmation during a workflow, such as flush required or move-cup prompts.</p>
          <div class="value-list" style="margin-bottom:16px;">
            <div class="value-item"><span>Suggested now</span><span>${statusSuggestsHostConfirm(liveStatus) ? "yes" : "no"}</span></div>
            <div class="value-item"><span>Current message</span><span>${escapeHtml(formatStatusCode(liveStatus.messageLabel, liveStatus.message))}</span></div>
          </div>
          <form data-action="diag-confirm-prompt">
            <input type="hidden" name="serial" value="${escapeHtml(machine.serial)}">
            <div class="row">
              <button type="submit">${escapeHtml(hostConfirmTitle(liveStatus))}</button>
            </div>
            <div class="muted tiny">Recovered app mapping: <code>HY</code> with 4 zero bytes.</div>
          </form>
        </article>

        <article class="diag-block">
          <h3>Raw characteristic access</h3>
          <form data-action="diag-raw-write">
            <input type="hidden" name="address" value="${escapeHtml(machine.address)}">
            <label class="label">Characteristic alias
              <select name="charAlias">
                <option value="tx">tx</option>
                <option value="ctrl">ctrl</option>
                <option value="rx">rx</option>
                <option value="aux1">aux1</option>
                <option value="aux2">aux2</option>
                <option value="name">name</option>
              </select>
            </label>
            <label class="label">Hex payload
              <textarea name="hex" placeholder="01 02 0A FF"></textarea>
            </label>
            <div class="row">
              <button type="submit">Write raw</button>
              <button type="button" class="secondary" data-action="diag-raw-read" data-address="${escapeHtml(machine.address)}">Read characteristic</button>
            </div>
          </form>
        </article>
      </section>

      <section class="grid two">
        <article class="diag-block">
          <h3>Send frame</h3>
          <form data-action="diag-send-frame">
            <input type="hidden" name="serial" value="${escapeHtml(machine.serial)}">
            <input type="hidden" name="address" value="${escapeHtml(machine.address)}">
            <div class="field-grid">
              <label class="label">Command
                <input type="text" name="command" maxlength="2" value="HV">
              </label>
              <label class="label">Payload hex
                <input type="text" name="payloadHex" placeholder="leave empty for none">
              </label>
              <label class="label">Wait ms
                <input type="number" name="waitMs" value="${defaults.waitMs}" min="0" step="100">
              </label>
              <label class="label">Session override
                <input type="text" name="sessionHex" placeholder="optional">
              </label>
            </div>
            <div class="row">
              <label><input type="checkbox" name="encrypt" checked> Encrypt</label>
              <label><input type="checkbox" name="pair" checked> Pair first</label>
              <label><input type="checkbox" name="reconnectAfterPair" checked> Reconnect</label>
              <label><input type="checkbox" name="useStoredSession"> Use stored session</label>
            </div>
            <div class="row">
              <button type="submit">Send frame</button>
            </div>
          </form>
        </article>

        <article class="diag-block">
          <h3>App-style probe</h3>
          <form data-action="diag-app-probe">
            <input type="hidden" name="serial" value="${escapeHtml(machine.serial)}">
            <input type="hidden" name="address" value="${escapeHtml(machine.address)}">
            <div class="field-grid">
              <label class="label">HR register
                <input type="number" name="hrRegisterId" value="200" min="0" max="65535">
              </label>
              <label class="label">Wait ms
                <input type="number" name="waitMs" value="3000" min="0" step="100">
              </label>
              <label class="label">Settle ms
                <input type="number" name="settleMs" value="500" min="0" step="100">
              </label>
              <label class="label">Session override
                <input type="text" name="sessionHex" placeholder="optional">
              </label>
            </div>
            <div class="row">
              <label><input type="checkbox" name="pair" checked> Pair first</label>
              <label><input type="checkbox" name="reconnectAfterPair" checked> Reconnect</label>
              <label><input type="checkbox" name="warmupPing" checked> Warmup with Hp</label>
              <label><input type="checkbox" name="encrypt" checked> Encrypt</label>
              <label><input type="checkbox" name="useStoredSession"> Use stored session</label>
            </div>
            <div class="row">
              <button type="submit">Run app probe</button>
              <button type="button" class="secondary" data-action="diag-settings-probe" data-address="${escapeHtml(machine.address)}">Settings probe</button>
              <button type="button" class="secondary" data-action="diag-stats-probe" data-address="${escapeHtml(machine.address)}">Stats probe</button>
            </div>
          </form>
        </article>
      </section>

      <section class="grid two">
        <article class="diag-block">
          <h3>Bridge logs</h3>
          <div class="row">
            <button data-action="diag-logs-refresh">Refresh logs</button>
            <button class="secondary" data-action="diag-logs-clear">Clear logs</button>
          </div>
          <pre>${logs}</pre>
        </article>
        <article class="diag-block">
          <h3>Last diagnostics response</h3>
          <pre>${output}</pre>
        </article>
      </section>
    `;
  }

  function renderSystemPage() {
    const history = state.status?.historyStorage || {};
    const timeConfig = state.status?.timeConfig || {};
    const timeMode = String(timeConfig.mode || "ntp");
    const ntpDisabled = timeMode === "no_time";
    const historyBudgetBytes = Math.max(0, Number(history.budgetBytes || 0));
    const historyBudgetMinBytes = Math.max(0, Number(history.budgetMinBytes || 0));
    const historyBudgetUpperBytes = Math.max(historyBudgetBytes, Number(history.budgetUpperBytes || 0));
    const historyTotalBytes = Math.max(0, Number(history.totalBytes || 0));
    const littleFsTotalBytes = Math.max(0, Number(state.status?.littleFsTotalBytes || 0));
    const littleFsUsedBytes = Math.max(0, Number(state.status?.littleFsUsedBytes || 0));
    const historyBudgetPercent = percentOf(historyBudgetBytes, historyBudgetUpperBytes);
    const historyUsagePercent = percentOf(historyTotalBytes, littleFsTotalBytes);
    const littleFsUsagePercent = percentOf(littleFsUsedBytes, littleFsTotalBytes);
    return sectionShell(
      "System",
      "Bridge-level controls stay separate from the coffee-machine dashboard: Wi-Fi, time, OTA, reboot, and store reset.",
      `
        <section class="grid two">
          <article class="panel">
            <h2 class="section-title">Wi-Fi</h2>
            <form data-action="save-wifi">
              <label class="label">SSID
                <input type="text" name="ssid" value="${escapeHtml(state.status?.staSsid || "")}" required>
              </label>
              <label class="label">Password
                <input type="password" name="password" value="">
              </label>
              <div class="row">
                <button type="submit">Save Wi-Fi</button>
              </div>
            </form>
          </article>

          <article class="panel">
            <h2 class="section-title">Firmware OTA</h2>
            <form data-action="ota-upload">
              <label class="label">Firmware file
                <input type="file" name="file" required>
              </label>
              <div class="row">
                <button type="submit">Upload OTA</button>
                <button type="button" class="secondary" data-action="reboot-bridge">Reboot bridge</button>
              </div>
            </form>
          </article>
        </section>

        <section class="panel">
          <div class="row" style="justify-content:space-between;align-items:flex-start;">
            <div>
              <h2 class="section-title">Time</h2>
              <div class="muted">Choose how the bridge should obtain wall-clock time for brew history and counter-history timestamps.</div>
            </div>
            <div class="badge-row">
              <span class="badge">${escapeHtml(timeModeLabel(timeMode))}</span>
              <span class="badge">${escapeHtml(timeSourceLabel(state.status))}</span>
            </div>
          </div>
          <div class="grid two" style="margin-top:16px;">
            <article class="setting-row">
              <form data-action="save-time-config">
                <label class="label">Mode
                  <select name="mode" data-action="time-mode-select">
                    <option value="ntp" ${timeMode === "ntp" ? "selected" : ""}>NTP</option>
                    <option value="no_time" ${timeMode === "no_time" ? "selected" : ""}>No time</option>
                  </select>
                </label>
                <div class="field-grid">
                  <label class="label">Primary NTP server
                    <input type="text" name="ntpServerPrimary" value="${escapeHtml(timeConfig.ntpServerPrimary || "pool.ntp.org")}" ${ntpDisabled ? "disabled" : ""}>
                  </label>
                  <label class="label">Secondary NTP server
                    <input type="text" name="ntpServerSecondary" value="${escapeHtml(timeConfig.ntpServerSecondary || "time.google.com")}" ${ntpDisabled ? "disabled" : ""}>
                  </label>
                  <label class="label">Tertiary NTP server
                    <input type="text" name="ntpServerTertiary" value="${escapeHtml(timeConfig.ntpServerTertiary || "time.cloudflare.com")}" ${ntpDisabled ? "disabled" : ""}>
                  </label>
                </div>
                <div class="${ntpDisabled ? "empty" : "muted"}" data-role="time-mode-warning">${escapeHtml(timeModeHelp(timeMode))}</div>
                <div class="row">
                  <button type="submit">Save time settings</button>
                </div>
              </form>
            </article>
            <article class="setting-row">
              <strong>Current clock</strong>
              <div class="value-list">
                <div class="value-item"><span>Source</span><span>${escapeHtml(timeSourceLabel(state.status))}</span></div>
                <div class="value-item"><span>Current UTC</span><span>${escapeHtml(state.status?.timeIsoUtc || (state.status?.timeAvailable ? String(state.status?.timeUnix || "") : "unavailable"))}</span></div>
                <div class="value-item"><span>Last NTP attempt</span><span>${escapeHtml(ntpAttemptLabel(state.status))}</span></div>
                <div class="value-item"><span>Last NTP success</span><span>${escapeHtml(ntpSuccessLabel(state.status))}</span></div>
                <div class="value-item"><span>NTP diagnostics</span><span>${escapeHtml(ntpDiagnosticLabel(state.status))}</span></div>
                ${ntpDiagnosticTargetLabel(state.status) ? `<div class="value-item"><span>Probe target</span><span>${escapeHtml(ntpDiagnosticTargetLabel(state.status))}</span></div>` : ""}
                ${state.status?.ntpDiagnosticRoundTripMs ? `<div class="value-item"><span>Probe RTT</span><span>${escapeHtml(String(state.status.ntpDiagnosticRoundTripMs))} ms</span></div>` : ""}
              </div>
            </article>
          </div>
        </section>

        <section class="panel">
          <div class="row" style="justify-content:space-between;align-items:flex-start;">
            <div>
              <h2 class="section-title">Brew history budget</h2>
              <div class="muted">Adjust the per-machine brew-history cap at runtime. The slider maximum is bounded by the mounted LittleFS size.</div>
            </div>
            <div class="badge-row">
              <span class="badge">${escapeHtml(formatBytes(historyBudgetBytes))} cap</span>
              <span class="badge">${escapeHtml(formatBytes(historyTotalBytes))} stored</span>
              <span class="badge">${escapeHtml(String(history.fileCount || 0))} files</span>
            </div>
          </div>
          <div class="grid two" style="margin-top:16px;">
            <article class="setting-row">
              <strong>Per-machine cap</strong>
              <div class="muted">Lowering the cap compacts existing history files immediately. Raising it only expands future headroom.</div>
              <div class="value-list">
                <div class="value-item"><span>Current</span><span>${escapeHtml(formatBytes(historyBudgetBytes))}</span></div>
                <div class="value-item"><span>Minimum</span><span>${escapeHtml(formatBytes(historyBudgetMinBytes))}</span></div>
                <div class="value-item"><span>Maximum</span><span>${escapeHtml(formatBytes(historyBudgetUpperBytes))}</span></div>
              </div>
              <div class="progress"><span style="width:${historyBudgetPercent}%;"></span></div>
              <form data-action="save-history-budget">
                <label class="label">Budget
                  <input type="range" name="budgetBytes" min="${escapeHtml(String(historyBudgetMinBytes || 0))}" max="${escapeHtml(String(historyBudgetUpperBytes || historyBudgetBytes || 0))}" step="4096" value="${escapeHtml(String(historyBudgetBytes || 0))}" data-action="history-budget-range">
                </label>
                <div class="row" style="justify-content:space-between;">
                  <span class="muted tiny">Selected: <output data-role="history-budget-output">${escapeHtml(formatBytes(historyBudgetBytes))}</output></span>
                  <button type="submit">Save history budget</button>
                </div>
              </form>
            </article>
            <article class="setting-row">
              <strong>LittleFS usage</strong>
              <div class="muted">History files share flash with recipe caches and other bridge data.</div>
              <div class="value-list">
                <div class="value-item"><span>History total</span><span>${escapeHtml(formatBytes(historyTotalBytes))}</span></div>
                <div class="value-item"><span>LittleFS used</span><span>${escapeHtml(formatBytes(littleFsUsedBytes))}</span></div>
                <div class="value-item"><span>LittleFS total</span><span>${escapeHtml(formatBytes(littleFsTotalBytes))}</span></div>
              </div>
              <div class="muted tiny">History files vs LittleFS total</div>
              <div class="progress"><span style="width:${historyUsagePercent}%;"></span></div>
              <div class="muted tiny" style="margin-top:10px;">All LittleFS files in use</div>
              <div class="progress"><span style="width:${littleFsUsagePercent}%;"></span></div>
              ${history.error ? `<div class="muted tiny">History stats error: ${escapeHtml(history.error)}</div>` : ""}
            </article>
          </div>
        </section>

        <section class="panel">
          <div class="row" style="justify-content:space-between;align-items:flex-start;">
            <div>
              <h2 class="section-title">Backup and restore</h2>
              <div class="muted">Backups include saved machines, the configured brew-history budget, and per-machine brew history. Wi-Fi credentials, recipe caches, and protocol-session caches are intentionally excluded.</div>
            </div>
          </div>
          <div class="grid two" style="margin-top:16px;">
            <article class="setting-row">
              <strong>Download backup</strong>
              <div class="muted">Exports the bridge state as an NDJSON bundle so you can archive it or restore it on another bridge.</div>
              <div class="row">
                <button type="button" data-action="download-backup">Download backup</button>
              </div>
            </article>
            <article class="setting-row">
              <strong>Restore backup</strong>
              <div class="muted">Restoring replaces saved machines and brew history on this bridge. Recipe caches and stored protocol sessions are cleared before import.</div>
              <form data-action="restore-backup">
                <label class="label">Backup file
                  <input type="file" name="file" accept=".ndjson,.jsonl,application/x-ndjson,application/jsonl" required>
                </label>
                <div class="row">
                  <button class="warn" type="submit">Restore backup</button>
                </div>
              </form>
            </article>
          </div>
        </section>

        <section class="panel">
          <h2 class="section-title">Saved machine store</h2>
          <p class="muted">Use this if the saved-machine schema changes or you want to wipe every remembered coffee machine from controller memory.</p>
          <div class="row">
            <button class="warn" data-action="reset-machines">Reset saved machines</button>
          </div>
        </section>

        <section class="panel">
          <h2 class="section-title">Raw bridge status</h2>
          <pre>${escapeHtml(pretty(state.status))}</pre>
        </section>
      `
    );
  }

  async function renderMachineRoute(serial, section) {
    const machine = findMachine(serial);
    if (!machine) {
      app.innerHTML = sectionShell("Machine not found", "The requested saved machine is not present in bridge storage.", `<div class="empty"><a href="#/dashboard"><strong>Return to dashboard</strong></a></div>`);
      return;
    }

    let summary = getMachineCache(serial).summary;
    if (!summary) {
      try {
        summary = await loadMachineSummary(serial, true);
      } catch (error) {
        setFlash(`Live summary unavailable for ${machine.alias || machine.serial}: ${error.message}`, "error");
      }
    }

    let body = "";
    try {
      if (section === "recipes") {
        const [recipesData, savedData] = await Promise.all([
          loadMachineRecipes(serial, false),
          loadSavedRecipes(serial, false).catch(() => ({ recipes: [] }))
        ]);
        body = renderRecipesSection(machine, recipesData, savedData);
      } else if (section === "saved") {
        body = renderSavedRecipesSection(machine, await loadSavedRecipes(serial, false));
      } else if (section === "history") {
        body = renderHistorySection(machine, await loadMachineHistory(serial, false));
      } else if (section === "stats") {
        const statsData = await loadMachineStats(serial, false);
        const statsHistory = await loadMachineStatsHistory(serial, false);
        body = renderStatsSection(machine, statsData, statsHistory);
      } else if (section === "features") {
        body = renderMachineFeaturesSection(machine, await loadMachineFeatures(serial, false));
      } else if (section === "settings") {
        body = renderSettingsSection(machine, await loadMachineSettings(serial, false));
      } else if (section === "diagnostics") {
        body = renderDiagnosticsSection(machine, summary);
      } else {
        body = `<section class="panel"><div class="empty">Unknown machine section.</div></section>`;
      }
    } catch (error) {
      setFlash(error.message || "Machine request failed.", "error");
      body = `
        <section class="panel">
          <div class="empty">The bridge could not load this machine section right now: ${escapeHtml(error.message || "request failed")}.</div>
        </section>
      `;
    }

    app.innerHTML = renderMachineHeader(machine, summary, section) + body;
  }

  async function renderRecipeRoute(serial, slot) {
    const machine = findMachine(serial);
    if (!machine) {
      app.innerHTML = sectionShell("Machine not found", "The requested saved machine is not present in bridge storage.", `<div class="empty"><a href="#/dashboard"><strong>Return to dashboard</strong></a></div>`);
      return;
    }

    let summary = getMachineCache(serial).summary;
    if (!summary) {
      try {
        summary = await loadMachineSummary(serial, true);
      } catch (error) {
        setFlash(`Live summary unavailable for ${machine.alias || machine.serial}: ${error.message}`, "error");
      }
    }

    try {
      const recipeData = await loadRecipeDetail(serial, slot, false);
      app.innerHTML = renderMachineHeader(machine, summary, "saved") + renderRecipeDetail(machine, recipeData, slot);
    } catch (error) {
      setFlash(error.message || "Recipe request failed.", "error");
      app.innerHTML = renderMachineHeader(machine, summary, "saved") + `
        <section class="panel">
          <div class="empty">The bridge could not load recipe slot ${escapeHtml(String(slot))}: ${escapeHtml(error.message || "request failed")}.</div>
        </section>
      `;
    }
  }

  async function renderStandardRecipeRoute(serial, selector) {
    const machine = findMachine(serial);
    if (!machine) {
      app.innerHTML = sectionShell("Machine not found", "The requested saved machine is not present in bridge storage.", `<div class="empty"><a href="#/dashboard"><strong>Return to dashboard</strong></a></div>`);
      return;
    }

    let summary = getMachineCache(serial).summary;
    if (!summary) {
      try {
        summary = await loadMachineSummary(serial, true);
      } catch (error) {
        setFlash(`Live summary unavailable for ${machine.alias || machine.serial}: ${error.message}`, "error");
      }
    }

    try {
      const recipeData = await loadStandardRecipeDetail(serial, selector, false);
      app.innerHTML = renderMachineHeader(machine, summary, "recipes") + renderStandardRecipeDetail(machine, recipeData, selector);
    } catch (error) {
      setFlash(error.message || "Recipe request failed.", "error");
      app.innerHTML = renderMachineHeader(machine, summary, "recipes") + `
        <section class="panel">
          <div class="empty">The bridge could not load standard recipe ${escapeHtml(String(selector))}: ${escapeHtml(error.message || "request failed")}.</div>
        </section>
      `;
    }
  }

  async function renderRoute(options = {}) {
    const showLoading = options.showLoading !== false;
    renderSidebar();
    const current = route();
    if (showLoading) {
      app.innerHTML = '<section class="panel"><div class="empty">Loading…</div></section>';
    }

    try {
      if (current.name === "dashboard") {
        app.innerHTML = renderDashboard();
        return;
      }
      if (current.name === "add") {
        app.innerHTML = renderAddPage();
        return;
      }
      if (current.name === "system") {
        app.innerHTML = renderSystemPage();
        return;
      }
      if (current.name === "machine") {
        await renderMachineRoute(current.serial, current.section);
        return;
      }
      if (current.name === "recipe-detail") {
        await renderRecipeRoute(current.serial, current.slot);
        return;
      }
      if (current.name === "standard-recipe-detail") {
        await renderStandardRecipeRoute(current.serial, current.selector);
        return;
      }

      app.innerHTML = renderDashboard();
    } catch (error) {
      setFlash(error.message || "Route render failed.", "error");
      app.innerHTML = `<section class="panel"><div class="empty">${escapeHtml(error.message || "Route render failed.")}</div></section>`;
    }
  }

  function parseCheckbox(form, name) {
    const field = form.elements[name];
    return !!field && !!field.checked;
  }

  function parseNumberField(form, name) {
    const field = form.elements[name];
    if (!field || !String(field.value).trim()) {
      return undefined;
    }
    return Number(field.value);
  }

  async function runWithFlash(message, work, successMessage) {
    setFlash(message);
    try {
      const result = await work();
      if (successMessage) {
        setFlash(successMessage, "ok");
      }
      return result;
    } catch (error) {
      setFlash(error.message || "Request failed", "error");
      throw error;
    }
  }

  document.body.addEventListener("click", async (event) => {
    const button = event.target.closest("[data-action]");
    if (!button) {
      return;
    }

    const action = button.dataset.action;
    if (action === "modal-cancel") {
      closeModal(false);
      return;
    }
    if (action === "modal-confirm") {
      closeModal(true);
      return;
    }
    try {
      if (action === "scan-devices") {
        const response = await runWithFlash("Scanning for nearby BLE devices…", () => api("/api/scan", { method: "POST", json: {} }), "Scan completed.");
        state.scanResults = (response.devices || []).slice().sort((a, b) => {
          if (a.likelySupported !== b.likelySupported) {
            return a.likelySupported ? -1 : 1;
          }
          return Number(b.rssi || -999) - Number(a.rssi || -999);
        });
        await refreshCore();
        await renderRoute();
      }

      if (action === "probe-device") {
        const address = button.dataset.address;
        const response = await runWithFlash(`Probing ${address}…`, () => api("/api/machines/probe", { json: { address } }), "Machine details loaded.");
        state.probedMachine = response.machine;
        await refreshCore();
        await renderRoute();
      }

      if (action === "clear-probe") {
        state.probedMachine = null;
        await renderRoute();
      }

      if (action === "download-backup") {
        setFlash("Starting backup download…");
        const link = document.createElement("a");
        link.href = "/api/backup/export";
        link.download = "";
        document.body.appendChild(link);
        link.click();
        link.remove();
      }

      if (action === "forget-machine") {
        const serial = button.dataset.serial;
        if (!window.confirm(`Forget saved machine ${serial}?`)) {
          return;
        }
        await runWithFlash(`Forgetting ${serial}…`, () => api(`/api/machines/${encodeURIComponent(serial)}`, { method: "DELETE" }), "Saved machine removed.");
        invalidateMachine(serial);
        await refreshCore();
        if (route().serial === serial) {
          location.hash = "#/dashboard";
          return;
        }
        await renderRoute();
      }

      if (action === "brew-standard") {
        const serial = button.dataset.serial;
        const selector = Number(button.dataset.selector);
        const response = await runWithFlash("Sending brew command…", () => api(`/api/machines/${encodeURIComponent(serial)}/brew`, { json: { selector, source: "ui" } }), "Brew command sent.");
        state.diagnostics.output = response;
        invalidateMachine(serial);
        await renderRoute();
      }

      if (action === "brew-history-again") {
        const serial = String(button.dataset.serial || "");
        const index = Number(button.dataset.index || -1);
        const cache = getMachineCache(serial);
        const entries = Array.isArray(cache.history?.entries) ? cache.history.entries : [];
        const entry = Number.isInteger(index) && index >= 0 ? entries[index] : null;
        if (!entry) {
          throw new Error("History entry is no longer available. Refresh history and try again.");
        }
        const payload = buildHistoryReplayPayload(entry);
        const confirmed = await openConfirmModal({
          title: "Brew again?",
          subtitle: "Replay this logged brew with the same saved values.",
          bodyHtml: renderHistoryReplayConfirmBody(entry),
          confirmLabel: "Brew again",
          cancelLabel: "Keep browsing"
        });
        if (!confirmed) {
          return;
        }
        const response = await runWithFlash(
          "Sending brew command…",
          () => api(`/api/machines/${encodeURIComponent(serial)}/brew`, { json: payload }),
          "Brew command sent."
        );
        state.diagnostics.output = response;
        invalidateMachine(serial);
        await renderRoute();
      }

      if (action === "confirm-machine") {
        const serial = button.dataset.serial;
        const response = await runWithFlash("Confirming machine prompt…", () => api(`/api/machines/${encodeURIComponent(serial)}/confirm`, {
          method: "POST",
          json: {}
        }), "Machine prompt confirmed.");
        state.diagnostics.output = response;
        getMachineCache(serial).summary = response;
        await renderRoute();
      }

      if (action === "refresh-standard-recipes") {
        const serial = button.dataset.serial;
        const response = await runWithFlash("Refreshing standard recipe cache…", () => api(`/api/machines/${encodeURIComponent(serial)}/recipes/refresh`, { method: "POST", json: {} }), "Recipe cache refreshed.");
        applyStandardRecipeRefresh(serial, response);
        await renderRoute();
      }

      if (action === "refresh-standard-recipe") {
        const serial = button.dataset.serial;
        const selector = String(button.dataset.selector || "");
        await runWithFlash(`Refreshing recipe ${selector}…`, () => loadStandardRecipeDetail(serial, selector, true), "Recipe values refreshed.");
        await renderRoute();
      }

      if (action === "refresh-saved-recipes") {
        const serial = button.dataset.serial;
        await runWithFlash("Refreshing saved recipes…", () => loadSavedRecipes(serial, true), "Saved recipes refreshed.");
        await renderRoute();
      }

      if (action === "refresh-saved-recipe") {
        const serial = button.dataset.serial;
        const slot = String(button.dataset.slot || "");
        await runWithFlash(`Refreshing saved recipe ${slot}…`, () => loadRecipeDetail(serial, slot, true), "Saved recipe refreshed.");
        await renderRoute();
      }

      if (action === "refresh-history") {
        const serial = button.dataset.serial;
        const cache = getMachineCache(serial);
        await runWithFlash("Refreshing brew history…", () => loadMachineHistory(serial, true, Number(cache.historyOffset || 0)), "Brew history refreshed.");
        await renderRoute();
      }

      if (action === "clear-history") {
        const serial = button.dataset.serial;
        if (!window.confirm(`Clear brew history for ${serial}?`)) {
          return;
        }
        await runWithFlash("Clearing brew history…", () => api(`/api/machines/${encodeURIComponent(serial)}/history/clear`, { method: "POST", json: {} }), "Brew history cleared.");
        const cache = getMachineCache(serial);
        cache.history = null;
        cache.historyOffset = 0;
        await renderRoute();
      }

      if (action === "history-prev-page" || action === "history-next-page") {
        const serial = button.dataset.serial;
        const offset = Number(button.dataset.offset || 0);
        await loadMachineHistory(serial, false, offset);
        await renderRoute();
      }

      if (action === "refresh-machine-stats") {
        const serial = button.dataset.serial;
        const cache = getMachineCache(serial);
        await runWithFlash("Refreshing machine statistics…", async () => {
          await loadMachineStats(serial, true);
          await loadMachineStatsHistory(serial, true, Number(cache.statsHistoryOffset || 0));
        }, "Machine statistics refreshed.");
        await renderRoute();
      }

      if (action === "refresh-machine-features") {
        const serial = button.dataset.serial;
        await runWithFlash("Refreshing machine features…", () => loadMachineFeatures(serial, true), "Machine features refreshed.");
        await renderRoute();
      }

      if (action === "clear-stats-history") {
        const serial = button.dataset.serial;
        if (!window.confirm(`Clear counter history for ${serial}?`)) {
          return;
        }
        await runWithFlash("Clearing counter history…", () => api(`/api/machines/${encodeURIComponent(serial)}/stats/history/clear`, { method: "POST", json: {} }), "Counter history cleared.");
        const cache = getMachineCache(serial);
        cache.statsHistory = null;
        cache.statsHistoryOffset = 0;
        await renderRoute();
      }

      if (action === "stats-history-prev-page" || action === "stats-history-next-page") {
        const serial = button.dataset.serial;
        const offset = Number(button.dataset.offset || 0);
        await loadMachineStatsHistory(serial, false, offset);
        await renderRoute();
      }

      if (action === "diag-session-clear") {
        const serial = button.dataset.serial;
        const address = button.dataset.address;
        const addressType = button.dataset.addressType;
        const response = await runWithFlash("Clearing machine protocol session…", () => api(`${PROTOCOL_API}/session`, {
          json: { clear: true, serial, address, addressType }
        }), "Protocol session cleared.");
        state.diagnostics.output = response;
        if (serial) {
          invalidateMachine(serial);
        }
        await refreshCore();
        await renderRoute();
      }

      if (action === "diag-settings-probe") {
        const address = button.dataset.address;
        const response = await runWithFlash("Running settings probe…", () => api(`${PROTOCOL_API}/settings-probe`, { json: { address, pair: true, reconnectAfterPair: true, encrypt: true, notificationMode: "notify", waitMs: 3000 } }), "Settings probe finished.");
        state.diagnostics.output = response;
        await renderRoute();
      }

      if (action === "diag-stats-probe") {
        const address = button.dataset.address;
        const response = await runWithFlash("Running stats probe…", () => api(`${PROTOCOL_API}/stats-probe`, { json: { address, pair: true, reconnectAfterPair: true, encrypt: true, notificationMode: "notify", waitMs: 3000 } }), "Stats probe finished.");
        state.diagnostics.output = response;
        await renderRoute();
      }

      if (action === "diag-raw-read") {
        const address = button.dataset.address;
        const aliasField = document.querySelector('[data-action="diag-raw-write"] select[name="charAlias"]');
        const charAlias = aliasField ? aliasField.value : "tx";
        const response = await runWithFlash(`Reading ${charAlias}…`, () => api(`${PROTOCOL_API}/raw-read`, { json: { address, charAlias } }), "Characteristic read complete.");
        state.diagnostics.output = response;
        await renderRoute();
      }

      if (action === "diag-logs-refresh") {
        const response = await runWithFlash("Refreshing bridge logs…", () => api("/api/logs"), "Logs refreshed.");
        state.diagnostics.logs = response;
        await renderRoute();
      }

      if (action === "diag-logs-clear") {
        await runWithFlash("Clearing bridge logs…", () => api("/api/logs/clear", { method: "POST", json: {} }), "Bridge logs cleared.");
        state.diagnostics.logs = await api("/api/logs");
        await renderRoute();
      }

      if (action === "reset-machines") {
        if (!window.confirm("Reset all saved machines on the bridge?")) {
          return;
        }
        await runWithFlash("Resetting saved machine store…", () => api("/api/machines/reset", { method: "POST", json: {} }), "Saved machine store reset.");
        state.probedMachine = null;
        state.machineCache = {};
        await refreshCore();
        location.hash = "#/dashboard";
      }

      if (action === "reboot-bridge") {
        if (!window.confirm("Reboot the bridge now?")) {
          return;
        }
        await runWithFlash("Rebooting bridge…", () => api("/api/reboot", { method: "POST", json: {} }), "Bridge reboot requested.");
      }
    } catch (error) {
      console.error(error);
    }
  });

  window.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && state.modal) {
      event.preventDefault();
      closeModal(false);
    }
  });

  document.body.addEventListener("change", async (event) => {
    const target = event.target;
    if (target instanceof HTMLInputElement && target.dataset.action === "toggle-show-coffee-machines") {
      state.showCoffeeMachines = !!target.checked;
      await renderRoute();
      return;
    }
    if (target instanceof HTMLSelectElement && target.dataset.action === "time-mode-select") {
      applyTimeModeFormState(target.form, target.value);
    }
  });

  document.body.addEventListener("input", (event) => {
    const target = event.target;
    if (!(target instanceof HTMLInputElement)) {
      return;
    }
    if (target.dataset.action === "history-budget-range") {
      const output = target.form?.querySelector('[data-role="history-budget-output"]');
      if (output) {
        output.textContent = formatBytes(Number(target.value || 0));
      }
    }
  });

  document.body.addEventListener("submit", async (event) => {
    const form = event.target;
    const action = form.dataset.action;
    if (!action) {
      return;
    }

    event.preventDefault();
    try {
      if (action === "save-probed-machine") {
        const alias = String(form.elements.alias.value || "").trim();
        const address = state.probedMachine?.address;
        const addressType = state.probedMachine?.addressType;
        if (!address) {
          throw new Error("No probed machine is loaded.");
        }
        const response = await runWithFlash("Saving machine…", () => api("/api/machines", { json: { address, addressType, alias } }), "Machine saved.");
        const serial = response.machine?.serial;
        state.probedMachine = null;
        await refreshCore();
        if (serial) {
          location.hash = machineHref(serial, "recipes");
        } else {
          await renderRoute();
        }
      }

      if (action === "save-manual-machine") {
        const alias = String(form.elements.alias.value || "").trim();
        const address = String(form.elements.address.value || "").trim().toUpperCase();
        const addressType = String(form.elements.addressType.value || "public");
        const serialInput = form.elements.serial;
        const modelInput = form.elements.model;
        const serial = String(serialInput?.value || "").trim();
        const model = String(modelInput?.value || "").trim();
        if (!address) {
          throw new Error("BLE address is required.");
        }
        if (!serial) {
          throw new Error("Serial number is required.");
        }
        const response = await runWithFlash(`Saving ${address}…`, () => api("/api/machines/manual", { json: { alias, address, addressType, serial, model } }), "Machine saved.");
        const savedSerial = response.machine?.serial;
        state.probedMachine = null;
        await refreshCore();
        if (savedSerial) {
          location.hash = machineHref(savedSerial, "recipes");
        } else {
          await renderRoute();
        }
      }

      if (action === "save-setting") {
        const serial = form.dataset.serial;
        const key = form.dataset.key;
        const value = String(form.elements.value.value || "");
        await runWithFlash(`Saving ${key}…`, () => api(`/api/machines/${encodeURIComponent(serial)}/settings`, { json: { key, value } }), "Setting updated.");
        getMachineCache(serial).settings = null;
        await renderRoute();
      }

      if (action === "factory-reset") {
        const serial = form.dataset.serial;
        const resetAction = form.dataset.resetAction;
        const label = resetAction === "factory_reset_settings" ? "settings" : "recipes";
        if (!window.confirm(`Reset ${label} to factory defaults on ${serial}?`)) {
          return;
        }
        await runWithFlash(`Resetting ${label}…`, () => api(`/api/machines/${encodeURIComponent(serial)}/settings`, { json: { action: resetAction } }), `Factory reset command sent for ${label}.`);
        invalidateMachine(serial);
        await renderRoute();
      }

      if (action === "save-recipe") {
        const serial = form.dataset.serial;
        const slot = form.dataset.slot;
        const payload = {};
        Array.from(form.elements).forEach((field) => {
          if (!field.name || field.disabled) {
            return;
          }
          if (field.name === "strengthBeans") {
            payload.strength = Number(field.value) - 1;
            return;
          }
          if (field.type === "number") {
            if (String(field.value).trim() !== "") {
              payload[field.name] = Number(field.value);
            }
            return;
          }
          if (field.tagName === "SELECT" || field.type === "text") {
            if (String(field.value).trim() !== "") {
              payload[field.name] = field.tagName === "SELECT" ? Number(field.value) : field.value;
            }
          }
        });
        await runWithFlash(`Saving recipe slot ${slot}…`, () => api(`/api/machines/${encodeURIComponent(serial)}/mycoffee/${slot}`, { json: payload }), "Recipe updated.");
        const cache = getMachineCache(serial);
        cache.savedRecipes = null;
        cache.recipeDetails = {};
        await renderRoute();
      }

      if (action === "brew-standard-custom") {
        const serial = form.dataset.serial;
        const selector = Number(form.dataset.selector);
        const payload = { selector, source: "ui" };
        Array.from(form.elements).forEach((field) => {
          if (!field.name || field.disabled) {
            return;
          }
          if (field.name === "strengthBeans") {
            payload.strengthBeans = Number(field.value);
            return;
          }
          if (field.type === "number") {
            if (String(field.value).trim() !== "") {
              payload[field.name] = Number(field.value);
            }
            return;
          }
          if (field.tagName === "SELECT" || field.type === "text") {
            if (String(field.value).trim() !== "") {
              payload[field.name] = field.tagName === "SELECT" ? Number(field.value) : field.value;
            }
          }
        });
        const response = await runWithFlash(`Brewing recipe ${selector}…`, () => api(`/api/machines/${encodeURIComponent(serial)}/brew`, { json: payload }), "Brew command sent.");
        state.diagnostics.output = response;
        invalidateMachine(serial);
        await renderRoute();
      }

      if (action === "diag-session-set") {
        const serial = String(form.elements.serial.value || "").trim();
        const response = await runWithFlash("Saving machine protocol session…", () => api(`${PROTOCOL_API}/session`, {
          json: {
            serial,
            address: String(form.elements.address.value || "").trim(),
            addressType: String(form.elements.addressType.value || "").trim(),
            sessionHex: String(form.elements.sessionHex.value || "").trim(),
            source: String(form.elements.source.value || "machine-diagnostics").trim()
          }
        }), "Protocol session updated.");
        state.diagnostics.output = response;
        if (serial) {
          invalidateMachine(serial);
        }
        await refreshCore();
        await renderRoute();
      }

      if (action === "diag-confirm-prompt") {
        const serial = String(form.elements.serial.value || "").trim();
        const response = await runWithFlash("Confirming machine prompt…", () => api(`/api/machines/${encodeURIComponent(serial)}/confirm`, {
          method: "POST",
          json: {}
        }), "Machine prompt confirmed.");
        state.diagnostics.output = response;
        getMachineCache(serial).summary = response;
        await renderRoute();
      }

      if (action === "diag-raw-write") {
        const response = await runWithFlash("Writing raw characteristic payload…", () => api(`${PROTOCOL_API}/raw-write`, {
          json: {
            address: form.elements.address.value,
            charAlias: form.elements.charAlias.value,
            hex: form.elements.hex.value,
            response: true,
            waitMs: 1000
          }
        }), "Raw characteristic write complete.");
        state.diagnostics.output = response;
        await renderRoute();
      }

      if (action === "diag-send-frame") {
        const serial = String(form.elements.serial.value || "").trim();
        const response = await runWithFlash("Sending frame…", () => api(`${PROTOCOL_API}/send-frame`, {
          json: {
            serial,
            address: form.elements.address.value,
            command: String(form.elements.command.value || "").trim(),
            payloadHex: String(form.elements.payloadHex.value || "").trim(),
            waitMs: parseNumberField(form, "waitMs") || 0,
            sessionHex: String(form.elements.sessionHex.value || "").trim(),
            encrypt: parseCheckbox(form, "encrypt"),
            pair: parseCheckbox(form, "pair"),
            reconnectAfterPair: parseCheckbox(form, "reconnectAfterPair"),
            useStoredSession: parseCheckbox(form, "useStoredSession"),
            notificationMode: "notify"
          }
        }), "Frame sent.");
        state.diagnostics.output = response;
        if (serial) {
          invalidateMachine(serial);
        }
        await renderRoute();
      }

      if (action === "diag-app-probe") {
        const serial = String(form.elements.serial.value || "").trim();
        const response = await runWithFlash("Running app-style probe…", () => api(`${PROTOCOL_API}/app-probe`, {
          json: {
            serial,
            address: form.elements.address.value,
            hrRegisterId: parseNumberField(form, "hrRegisterId") || 200,
            waitMs: parseNumberField(form, "waitMs") || 3000,
            settleMs: parseNumberField(form, "settleMs") || 500,
            sessionHex: String(form.elements.sessionHex.value || "").trim(),
            pair: parseCheckbox(form, "pair"),
            reconnectAfterPair: parseCheckbox(form, "reconnectAfterPair"),
            warmupPing: parseCheckbox(form, "warmupPing"),
            encrypt: parseCheckbox(form, "encrypt"),
            useStoredSession: parseCheckbox(form, "useStoredSession"),
            notificationMode: "notify"
          }
        }), "App-style probe finished.");
        state.diagnostics.output = response;
        if (serial) {
          invalidateMachine(serial);
        }
        await renderRoute();
      }

      if (action === "save-wifi") {
        await runWithFlash("Saving Wi-Fi credentials…", () => api("/api/wifi/save", {
          json: {
            ssid: String(form.elements.ssid.value || "").trim(),
            password: String(form.elements.password.value || "")
          }
        }), "Wi-Fi credentials saved.");
        await refreshCore();
        await renderRoute();
      }

      if (action === "save-time-config") {
        await runWithFlash("Saving time settings…", () => api("/api/time/config", {
          json: {
            mode: String(form.elements.mode.value || "ntp").trim(),
            ntpServerPrimary: String(form.elements.ntpServerPrimary?.value || "").trim(),
            ntpServerSecondary: String(form.elements.ntpServerSecondary?.value || "").trim(),
            ntpServerTertiary: String(form.elements.ntpServerTertiary?.value || "").trim()
          }
        }), "Time settings saved.");
        await refreshCore();
        await renderRoute();
      }

      if (action === "ota-upload") {
        const file = form.elements.file.files[0];
        if (!file) {
          throw new Error("Choose a firmware file first.");
        }
        const body = new FormData();
        body.append("file", file);
        await runWithFlash("Uploading OTA firmware…", () => api("/api/ota", { method: "POST", body }), "OTA upload accepted.");
      }

      if (action === "save-history-budget") {
        const budgetBytes = Number(form.elements.budgetBytes.value || 0);
        await runWithFlash("Saving brew history budget…", () => api("/api/history/config", {
          method: "POST",
          json: { budgetBytes }
        }), "Brew history budget saved.");
        Object.values(state.machineCache || {}).forEach((cache) => {
          cache.history = null;
          cache.historyOffset = 0;
        });
        await refreshCore();
        await renderRoute();
      }

      if (action === "restore-backup") {
        const file = form.elements.file.files && form.elements.file.files[0];
        if (!file) {
          throw new Error("Choose a backup file first.");
        }
        if (!window.confirm("Restore this backup and replace saved machines and brew history on the bridge? Recipe caches and stored protocol sessions will be cleared.")) {
          return;
        }
        const body = new FormData();
        body.append("file", file);
        const response = await runWithFlash("Restoring bridge backup…", () => api("/api/backup/restore", {
          method: "POST",
          body
        }), "Bridge backup restored.");
        state.probedMachine = null;
        state.machineCache = {};
        state.diagnostics.output = response;
        await refreshCore();
        await renderRoute();
      }
    } catch (error) {
      console.error(error);
    }
  });

  window.addEventListener("hashchange", () => {
    closeModal(false);
    void renderRoute();
  });

  async function boot() {
    try {
      await refreshCore();
      renderSidebar();
      await renderRoute();
      setFlash("Bridge ready.", "ok");
    } catch (error) {
      setFlash(error.message || "Failed to load bridge status.", "error");
      app.innerHTML = '<section class="panel"><div class="empty">The bridge did not answer the first status request.</div></section>';
    }

    setInterval(async () => {
      try {
        await refreshCore();
        renderSidebar();
        const current = route();
        if (current.name === "dashboard") {
          await renderRoute({ showLoading: false });
        }
      } catch (error) {
        console.error(error);
      }
    }, 15000);
  }

  void boot();
</script>
</body>
</html>
)HTML";

} // namespace web_ui
