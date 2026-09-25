//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Painel de saude do servidor, no canto superior direito.
//
//  Le GET /api/health em intervalo. Nao usa EventSource: o painel quer UM numero por
//  segundo, e segurar uma conexao aberta por aba so para isso sai caro. SSE nesta casa e
//  para job longo com progresso.
//
//  Os tres numeros sao do PROCESSO servidor, nao da maquina -- ver health_monitor.h.

const BASE = import.meta.url.replace(/\/[^\/]*$/, '');

const INTERVALO_MS   = 1000;
const INTERVALO_ERRO = 5000;   // servidor fora do ar: espaca as tentativas
const HISTORICO      = 30;     // amostras guardadas para a tendencia de blocos vivos

function bytes_humano(n)
{
    if (!n && n !== 0) return '--';
    const u = ['B', 'KB', 'MB', 'GB', 'TB'];
    let i = 0, v = Number(n);
    while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
    return (v < 10 && i > 0 ? v.toFixed(1) : Math.round(v)) + ' ' + u[i];
}

//  O log do mem_leak_watch e TSV e cada linha traz a pilha inteira da alocacao. As tres
//  primeiras molduras sao sempre o alocador (span_create <- memop_refill <- memop_alloc_raw)
//  e se repetem em todas as linhas -- mostrar isso cru enche a tela sem informar nada.
//  O que interessa e a primeira moldura FORA do xplatbase: e ali que esta o codigo que
//  alocou e nao liberou.
function molduras(site)
{
    // "func+0x12 (caminho:linha) <- func2+0x34 (caminho:linha) <- ..."
    return site.split('<-').map(x => x.trim()).filter(Boolean).map(x =>
    {
        const m = x.match(/^(.+?)\+0x[0-9a-f]+\s*\((.*):(\d+)\)$/i);
        if (!m) return { nome: x.replace(/\+0x[0-9a-f]+$/i, ''), arquivo: '', linha: '' };
        const caminho = m[2];
        return {
            nome: m[1],
            arquivo: caminho.split(/[\\/]/).pop(),
            linha: m[3],
            xplat: /submodules[\\/]xplatbase/i.test(caminho)
        };
    });
}

function escapa(t)
{
    return String(t).replace(/[&<>"]/g, c => ({ '&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;' }[c]));
}

function formata_relatorio(txt)
{
    const linhas = txt.split('\n').filter(l => l.trim() && !l.startsWith('ts_ms'));
    if (!linhas.length) return '<div class="vazio">A varredura nao apontou nenhum candidato.</div>';

    const partes = [];
    for (const l of linhas)
    {
        const c = l.split('\t');
        if (c.length < 8) continue;

        const [, nivel, , , tam, usados, idade] = c;
        const fr = molduras(c[7]);

        // primeira moldura fora do xplatbase; se nao houver, a primeira depois do alocador
        let alvo = fr.find(f => f.arquivo && !f.xplat);
        if (!alvo) alvo = fr[3] || fr[fr.length - 1];

        const resto = fr.filter(f => f !== alvo && f.arquivo && !f.xplat)
                        .slice(0, 2).map(f => f.nome).join(' <- ');

        partes.push(
            `<div class="item ${nivel === 'CRITICAL' ? 'crit' : ''}">` +
              `<div class="origem">${escapa(alvo ? alvo.nome : '(sem simbolo)')}` +
                (alvo && alvo.arquivo ? `<span class="arq">${escapa(alvo.arquivo)}:${escapa(alvo.linha)}</span>` : '') +
              `</div>` +
              (resto ? `<div class="pilha">${escapa(resto)}</div>` : '') +
              `<div class="nums">${escapa(tam)} B &middot; ${escapa(usados)} em uso &middot; ${escapa(idade)} ms</div>` +
            `</div>`);
    }
    return `<div class="resumo">${partes.length} ${partes.length === 1 ? "candidato" : "candidatos"}</div>` + partes.join('');
}


export class HealthBox extends HTMLElement
{
    #timer   = null;
    #vivos   = [];      // historico de blocos vivos
    #pico    = 64 * 1024 * 1024;   // escala da barra de memoria, cresce conforme observado
    #parado  = false;

    constructor()
    {
        super();
        this.attachShadow({ mode: 'open' });
    }

    async connectedCallback()
    {
        const [html, css] = await Promise.all([
            fetch(`${BASE}/health-box.html`).then(r => r.text()),
            fetch(`${BASE}/health-box.css`).then(r => r.text())
        ]);
        this.shadowRoot.innerHTML = `<style>${css}</style>${html}`;

        const $ = id => this.shadowRoot.getElementById(id);
        this.el = {
            painel: $('painel'), topo: $('topo'), seta: $('seta'), led: $('led'),
            cpuB: $('cpu-barra'), cpuV: $('cpu-valor'),
            gpuB: $('gpu-barra'), gpuV: $('gpu-valor'),
            memB: $('mem-barra'), memV: $('mem-valor'),
            vivos: $('vivos'), tend: $('tend'), reservado: $('reservado'),
            gpuVideo: $('gpu-video'), gpuVideoLinha: $('gpu-video-linha'),
            btnScan: $('btn-scan'), scanAviso: $('scan-aviso'), log: $('log'),
            leakMotivo: $('leak-motivo')
        };

        this.el.topo.addEventListener('click', () => this.#alterna());
        this.el.btnScan.addEventListener('click', () => this.#varrer());

        this.#ciclo();
    }

    disconnectedCallback()
    {
        this.#parado = true;
        if (this.#timer) { clearTimeout(this.#timer); this.#timer = null; }
    }

    #alterna()
    {
        const fechado = this.el.painel.classList.toggle('fechado');
        this.el.seta.innerHTML = fechado ? '&#9660;' : '&#9650;';
    }

    async #ciclo()
    {
        if (this.#parado) return;

        let proximo = INTERVALO_MS;
        try
        {
            const r = await fetch('/api/health', { cache: 'no-store' });
            if (!r.ok) throw new Error(`HTTP ${r.status}`);
            this.#pinta(await r.json());
            this.el.led.classList.remove('off');
        }
        catch (e)
        {
            this.el.led.classList.add('off');
            this.el.cpuV.textContent = this.el.gpuV.textContent = this.el.memV.textContent = '--';
            proximo = INTERVALO_ERRO;
        }

        this.#timer = setTimeout(() => this.#ciclo(), proximo);
    }

    #barra(el, pct)
    {
        const p = Math.max(0, Math.min(100, pct));
        el.style.width = p + '%';
        el.classList.toggle('medio', p >= 60 && p < 85);
        el.classList.toggle('alto',  p >= 85);
    }

    #pinta(d)
    {
        /* ---- CPU ---- */
        this.#barra(this.el.cpuB, d.cpu.percent);
        this.el.cpuV.textContent = d.cpu.percent.toFixed(1) + '%';
        this.el.cpuV.title = `${d.cpu.cores} nucleos logicos`;

        /* ---- GPU ---- */
        if (d.gpu.available)
        {
            this.#barra(this.el.gpuB, d.gpu.percent);
            this.el.gpuV.textContent = d.gpu.percent.toFixed(1) + '%';
            this.el.gpuV.classList.remove('nd');
            this.el.gpuVideoLinha.style.display = '';
            this.el.gpuVideo.textContent =
                d.gpu.videoPercent.toFixed(1) + '%' +
                (d.gpu.memoryBytes ? ' / ' + bytes_humano(d.gpu.memoryBytes) : '');
        }
        else
        {
            this.#barra(this.el.gpuB, 0);
            this.el.gpuV.textContent = 'n/d';
            this.el.gpuV.classList.add('nd');
            this.el.gpuV.title = 'sem fonte de GPU nesta plataforma';
            this.el.gpuVideoLinha.style.display = 'none';
        }

        /* ---- Memoria ----
           Nao ha "100%" natural para o RSS de um processo, entao a barra e relativa ao
           MAIOR valor ja visto nesta sessao de painel. Serve para enxergar crescimento,
           que e o que interessa; nao e fracao da RAM da maquina. */
        const rss = Number(d.memory.rssBytes);
        if (rss > this.#pico) this.#pico = rss;
        this.#barra(this.el.memB, (rss / this.#pico) * 100);
        this.el.memV.textContent = bytes_humano(rss);
        this.el.memV.title = `pico observado: ${bytes_humano(this.#pico)}`;

        /* ---- blocos vivos e tendencia ----
           O numero absoluto conta o processo inteiro e nao diz muito sozinho. O que acusa
           acumulo e a INCLINACAO: subir de forma sustentada com o servidor ocioso. */
        const vivos = Number(d.memory.poolLiveBlocks);
        this.el.vivos.textContent = vivos.toLocaleString('pt-BR');

        this.#vivos.push(vivos);
        if (this.#vivos.length > HISTORICO) this.#vivos.shift();

        if (this.#vivos.length >= 5)
        {
            const delta = vivos - this.#vivos[0];
            const sinal = delta > 0 ? '+' : '';
            this.el.tend.textContent = `(${sinal}${delta.toLocaleString('pt-BR')} em ${this.#vivos.length}s)`;
            this.el.tend.classList.toggle('sobe',  delta > 0);
            this.el.tend.classList.toggle('desce', delta < 0);
        }

        this.el.reservado.textContent = bytes_humano(d.memory.poolReservedBytes);

        /* ---- alarme de vazamento ----
           Vem pronto do servidor: e a leitura barata dos contadores do pool, sem suspender
           thread nenhuma. O botao pisca so para chamar atencao; pagar pela varredura
           detalhada continua sendo escolha de quem esta olhando. */
        this.#alarme(d.leak);

        /* ---- varredura ---- */
        if (!d.leakWatch.available)
        {
            this.el.btnScan.disabled = true;
            this.el.scanAviso.textContent = 'so Windows';
        }
    }

    #alarme(k)
    {
        const b = this.el.btnScan, m = this.el.leakMotivo;
        b.classList.remove('alarme', 'observar');
        m.classList.remove('alarme', 'observar', 'ok');

        if (!k)
        {
            m.textContent = '';
            return;
        }

        // Janela ainda enchendo: nao ha o que afirmar, e melhor dizer isso do que
        // mostrar um "ok" que ninguem mediu.
        if (k.level === 0 && k.windowSec < 60)
        {
            m.classList.add('ok');
            m.textContent = `medindo o piso de memoria (${k.windowSec}s de 60s)`;
            return;
        }

        const porMin = Number(k.floorPerMin).toLocaleString('pt-BR');

        if (k.level === 2)
        {
            b.classList.add('alarme');
            m.classList.add('alarme');
            m.textContent = `piso de memoria subindo ${porMin} blocos/min -- memoria entrou e nao voltou`;
            b.title = 'O alarme barato acusou acumulo. Clique para a varredura detalhada.';
        }
        else if (k.level === 1)
        {
            b.classList.add('observar');
            m.classList.add('observar');
            m.textContent = `piso de memoria subindo ${porMin} blocos/min -- observando`;
        }
        else
        {
            m.classList.add('ok');
            m.textContent = `piso estavel (${porMin} blocos/min em ${k.windowSec}s)`;
        }
    }

    //  Pede confirmacao de proposito. A varredura suspende as threads uma a uma, o que a
    //  Microsoft documenta como arriscado fora de um depurador. Medido aqui: num processo
    //  saudavel ela volta em ~0,4 s e o servidor continua atendendo; num processo com DUAS
    //  instancias do xplatbase (ver health_controller.c) ela travou o servidor de vez.
    async #varrer()
    {
        const ok = confirm(
            'A varredura suspende as threads do servidor uma a uma para percorrer o pool.\n\n' +
            'Num processo saudavel volta em menos de um segundo. Se houver mais de uma\n' +
            'instancia do xplatbase no processo, ja travou o servidor em teste.\n\n' +
            'Continuar?');
        if (!ok) return;

        this.el.btnScan.disabled = true;
        this.el.scanAviso.textContent = 'varrendo...';
        this.el.log.textContent = '';
        try
        {
            const r = await fetch('/api/health/leak-scan', { method: 'POST' });
            const d = await r.json();

            if (d.ran)
            {
                this.el.log.innerHTML = formata_relatorio(d.log);
                this.el.painel.classList.add('largo');
            }
            else
            {
                this.el.log.textContent =
                      'A varredura rodou, mas nao veio texto do mem_leak_watch.log.\n'
                    + 'Ele fica AO LADO DO EXECUTAVEL (x64/Debug), nao no diretorio de trabalho.';
            }
            this.el.scanAviso.textContent = d.ran ? `${bytes_humano(d.bytes)} de log` : '';
        }
        catch (e)
        {
            this.el.scanAviso.textContent = 'falhou';
        }
        this.el.btnScan.disabled = false;
    }
}

customElements.define('health-box', HealthBox);
