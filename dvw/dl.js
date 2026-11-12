(function () {
  'use strict';

  const CHECK_TEXT = 'DVW-DOWNLOAD-V1';
  const PART_SIZE = 16 * 1024 * 1024;

  function fromBase64(text) {
    return Uint8Array.from(atob(text), character => character.charCodeAt(0));
  }

  function validateManifest(manifest) {
    if (manifest.format !== 1 || typeof manifest.name !== 'string' || !manifest.name ||
        !Number.isSafeInteger(manifest.size) || manifest.size < 0 ||
        !/^[0-9a-f]{64}$/.test(manifest.sha256) ||
        !Number.isFinite(Date.parse(manifest.date)) ||
        manifest.kdf.name !== 'PBKDF2' || manifest.kdf.hash !== 'SHA-256' ||
        manifest.kdf.iterations !== 600000 || fromBase64(manifest.kdf.salt).length !== 16 ||
        fromBase64(manifest.check.iv).length !== 12 ||
        fromBase64(manifest.check.ct).length !== CHECK_TEXT.length + 16 ||
        !Array.isArray(manifest.parts) ||
        manifest.parts.length !== Math.ceil(manifest.size / PART_SIZE)) {
      throw new Error('Invalid manifest');
    }
    manifest.parts.forEach((part, index) => {
      const plainSize = Math.min(PART_SIZE, manifest.size - index * PART_SIZE);
      if (!/^parts\/[0-9a-f]{16}\.bin$/.test(part.path) ||
          part.size !== plainSize + 16 || fromBase64(part.iv).length !== 12) {
        throw new Error('Invalid part');
      }
    });
    return manifest;
  }

  async function deriveKey(password, kdf) {
    const material = await globalThis.crypto.subtle.importKey(
      'raw', new TextEncoder().encode(password), 'PBKDF2', false, ['deriveKey']
    );
    return globalThis.crypto.subtle.deriveKey(
      { name: kdf.name, hash: kdf.hash, iterations: kdf.iterations, salt: fromBase64(kdf.salt) },
      material, { name: 'AES-GCM', length: 256 }, false, ['decrypt']
    );
  }

  async function decryptCheck(key, check) {
    const plain = await globalThis.crypto.subtle.decrypt(
      { name: 'AES-GCM', iv: fromBase64(check.iv) }, key, fromBase64(check.ct)
    );
    if (new TextDecoder().decode(plain) !== CHECK_TEXT) {
      throw new Error('Invalid password check');
    }
  }

  async function decryptPart(key, part, ciphertext) {
    if (ciphertext.byteLength !== part.size) throw new Error('Part size mismatch');
    return new Uint8Array(await globalThis.crypto.subtle.decrypt(
      { name: 'AES-GCM', iv: fromBase64(part.iv) }, key, ciphertext
    ));
  }

  async function verifyFile(chunks, manifest) {
    const size = chunks.reduce((total, chunk) => total + chunk.byteLength, 0);
    if (size !== manifest.size) throw new Error('File size mismatch');
    const bytes = new Uint8Array(size);
    let offset = 0;
    for (const chunk of chunks) {
      bytes.set(chunk, offset);
      offset += chunk.byteLength;
    }
    const digest = new Uint8Array(await globalThis.crypto.subtle.digest('SHA-256', bytes));
    const hex = Array.from(digest, byte => byte.toString(16).padStart(2, '0')).join('');
    if (hex !== manifest.sha256) throw new Error('File SHA-256 mismatch');
    return bytes;
  }

  if (typeof module !== 'undefined') {
    module.exports = { validateManifest, deriveKey, decryptCheck, decryptPart, verifyFile };
  }

  function attach() {
    const form = document.getElementById('dl-form');
    if (!form) return;
    const base = form.dataset.base;
    const password = document.getElementById('dl-password');
    const button = document.getElementById('dl-button');
    const label = document.getElementById('dl-label');
    const meta = document.getElementById('dl-meta');
    const progress = document.getElementById('dl-progress');
    const error = document.getElementById('dl-error');
    const originalLabel = label.textContent;
    let cachedFile = null;

    function showMeta(manifest) {
      const fields = new Intl.DateTimeFormat('en-CA', {
        timeZone: 'Asia/Seoul', year: 'numeric', month: '2-digit', day: '2-digit',
        hour: '2-digit', minute: '2-digit', hourCycle: 'h23'
      }).formatToParts(new Date(manifest.date));
      const date = Object.fromEntries(fields.map(field => [field.type, field.value]));
      const text = [
        (manifest.size / 1048576).toFixed(1) + ' MB',
        `${date.year}-${date.month}-${date.day} ${date.hour}:${date.minute}`
      ];
      meta.replaceChildren(...text.map(value => {
        const span = document.createElement('span');
        span.className = 'nb';
        span.textContent = value;
        return span;
      }));
    }

    async function fetchManifest() {
      const response = await fetch(base + 'manifest.json?t=' + Date.now(), { cache: 'no-store' });
      if (!response.ok) throw new Error('Manifest HTTP error');
      const manifest = validateManifest(await response.json());
      showMeta(manifest);
      return manifest;
    }

    function clearError() {
      error.textContent = '';
      password.removeAttribute('aria-invalid');
    }

    function invalidPassword(message, select) {
      error.textContent = message;
      password.setAttribute('aria-invalid', 'true');
      password.focus();
      if (select) password.select();
    }

    function showProgress(fraction) {
      const value = Math.max(0, Math.min(1, fraction));
      progress.style.setProperty('--p', String(value));
      label.textContent = '받는 중 ' + Math.floor(value * 100) + '%';
    }

    async function fetchPart(part, onBytes) {
      const response = await fetch(base + part.path);
      if (!response.ok || !response.body) throw new Error('Part HTTP error');
      const reader = response.body.getReader();
      const bytes = new Uint8Array(part.size);
      let offset = 0;
      try {
        while (true) {
          const { done, value } = await reader.read();
          if (done) break;
          if (offset + value.byteLength > part.size) {
            await reader.cancel();
            throw new Error('Part size mismatch');
          }
          bytes.set(value, offset);
          offset += value.byteLength;
          onBytes(value.byteLength);
        }
      } finally {
        reader.releaseLock();
      }
      if (offset !== part.size) throw new Error('Part size mismatch');
      return bytes;
    }

    function saveFile(blob, name) {
      const url = URL.createObjectURL(blob);
      const link = document.createElement('a');
      link.href = url;
      link.download = name;
      document.body.appendChild(link);
      link.click();
      link.remove();
      setTimeout(() => URL.revokeObjectURL(url), 60000);
    }

    password.addEventListener('input', clearError);
    // index.html ships the button disabled: before this script attaches, Enter must not do the
    // browser's default GET submit (that would put the password in the address bar and history).
    button.disabled = false;
    fetchManifest().catch(() => {});

    form.addEventListener('submit', async event => {
      event.preventDefault();
      if (button.disabled) return;
      clearError();
      if (!password.value) {
        invalidPassword('비밀번호를 입력하세요.', false);
        return;
      }
      if (!globalThis.crypto || !globalThis.crypto.subtle) {
        error.textContent = '이 브라우저에서는 받을 수 없습니다.';
        return;
      }
      button.disabled = true;
      button.setAttribute('aria-busy', 'true');
      label.textContent = '확인 중…';
      try {
        const manifest = await fetchManifest();
        const key = await deriveKey(password.value, manifest.kdf);
        try {
          await decryptCheck(key, manifest.check);
        } catch (failure) {
          invalidPassword('비밀번호가 틀렸습니다.', true);
          return;
        }
        if (!cachedFile || cachedFile.sha256 !== manifest.sha256) {
          const total = manifest.parts.reduce((size, part) => size + part.size, 0);
          const chunks = [];
          let received = 0;
          showProgress(0);
          for (const part of manifest.parts) {
            const ciphertext = await fetchPart(part, size => {
              received += size;
              showProgress(received / total);
            });
            chunks.push(await decryptPart(key, part, ciphertext));
          }
          const bytes = await verifyFile(chunks, manifest);
          cachedFile = {
            sha256: manifest.sha256,
            blob: new Blob([bytes], { type: 'application/octet-stream' })
          };
        }
        if (cachedFile.blob.size !== manifest.size) throw new Error('File size mismatch');
        saveFile(cachedFile.blob, manifest.name);
      } catch (failure) {
        error.textContent = '받지 못했습니다. 다시 눌러 주세요.';
      } finally {
        button.disabled = false;
        button.removeAttribute('aria-busy');
        label.textContent = originalLabel;
        progress.style.setProperty('--p', '0');
      }
    });
  }

  if (typeof document !== 'undefined') {
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', attach);
    else attach();
  }
})();
