//  MIT License – Modified for Mandatory Attribution
//
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files,
//  to use, copy, modify, merge, publish, distribute, and sublicense the software, including for commercial purposes, provided that:
//
//     01. The original author’s credit is retained in all copies of the source code;
//     02. The original author’s credit is included in any code generated, derived, or distributed from this software, including templates, libraries, or code - generating scripts.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.


export const PlayerControlOptions = Object.freeze({ NONE: '', STOP: 'STOP', PLAY: 'PLAY', PAUSE: 'PAUSE', FORWARD: "FORWARD", BACK: "BACK", FORWARD_STEP: "FORWARD-STEP", BACK_STEP: "BACK-STEP"  });

export class VideoPlayer extends HTMLElement
{
    VIDEO_URL_DEMO = "https://samplelib.com/lib/preview/mp4/sample-5s.mp4"; // 5 segundos, H.264 MP4

    video = null;
    object_url = null;
    hls = null;

    constructor()
    {
        super();
        this.attachShadow({ mode: 'open' });
    }


    async connectedCallback()
    {
        const html_url = new URL('./video-player.html', import.meta.url);
        const css_url  = new URL('./video-player.css', import.meta.url);
        // no-store: garante HTML/CSS sempre frescos (evita ver versao antiga em cache apos editar).
        const html     = await fetch(html_url, { cache: 'no-store' }).then(r => r.text());
        const css      = await fetch(css_url,  { cache: 'no-store' }).then(r => r.text());

        this.shadowRoot.innerHTML = `<style>${css}</style>${html}`;

        this.video                 = this.shadowRoot.querySelector('#player');
        this.source_button         = this.shadowRoot.querySelector('#player-open-file');
        this.frag_start_btn        = this.shadowRoot.querySelector('#frag-start');
        this.frag_hint             = this.shadowRoot.querySelector('#frag-hint');
        this.file_input            = this.shadowRoot.querySelector('#player-file');
        this.status_field           = this.shadowRoot.querySelector('#processing-status');
        this.frag_time              = this.shadowRoot.querySelector('#frag-time');
        this.mute_btn               = this.shadowRoot.querySelector('#mute');
        this.volume_slider          = this.shadowRoot.querySelector('#volume');
        this.player_shell           = this.shadowRoot.querySelector('#player-shell');
        this.fullscreen_btn         = this.shadowRoot.querySelector('#fullscreen');
        this.frag_head              = this.shadowRoot.querySelector('#frag-head');
        this.frag_head_pct          = this.shadowRoot.querySelector('#frag-head-pct');
        this.frag_head_fill         = this.shadowRoot.querySelector('#frag-head-fill');
        this.frag_head_decorrido    = this.shadowRoot.querySelector('#frag-head-decorrido');
        this.frag_head_total        = this.shadowRoot.querySelector('#frag-head-total');
        this.session_select         = this.shadowRoot.querySelector('#session-select');
        this.session_new_btn        = this.shadowRoot.querySelector('#session-new');
        this.session_name_field     = this.shadowRoot.querySelector('#session-name');
        this.session_delete_btn     = this.shadowRoot.querySelector('#session-delete');
        this.session_cancel_btn     = this.shadowRoot.querySelector('#session-cancel');
        this.session_info           = this.shadowRoot.querySelector('#session-info');
        this.in_device              = this.shadowRoot.querySelector('#in-device');
        this.in_codec               = this.shadowRoot.querySelector('#in-codec');
        this.in_resolution          = this.shadowRoot.querySelector('#in-resolution');
        this.in_fps                 = this.shadowRoot.querySelector('#in-fps');
        this.in_note                = this.shadowRoot.querySelector('#in-note');
        this.out_codec              = this.shadowRoot.querySelector('#out-codec');
        this.out_resolution         = this.shadowRoot.querySelector('#out-resolution');
        this.out_fps                = this.shadowRoot.querySelector('#out-fps');
        this.out_bitrate            = this.shadowRoot.querySelector('#out-bitrate');
        this.out_note               = this.shadowRoot.querySelector('#out-note');
        this.resolution_select      = this.shadowRoot.querySelector('#hls-resolution');
        this.progress_wrap          = this.shadowRoot.querySelector('#hls-progress');
        this.progress_bar           = this.shadowRoot.querySelector('#progress-bar');
        this.progress_fill          = this.shadowRoot.querySelector('#progress-fill');
        this.progress_ticks         = this.shadowRoot.querySelector('#progress-ticks');
        this.progress_tooltip       = this.shadowRoot.querySelector('#progress-tooltip');
        this.fragment_counter       = this.shadowRoot.querySelector('#fragment-counter');
        this.seek_row               = this.shadowRoot.querySelector('#player-seek');
        this.seek_bar               = this.shadowRoot.querySelector('#seek-bar');
        this.seek_fill              = this.shadowRoot.querySelector('#seek-fill');
        this.seek_handle            = this.shadowRoot.querySelector('#seek-handle');
        this.seek_current           = this.shadowRoot.querySelector('#seek-current');
        this.seek_total             = this.shadowRoot.querySelector('#seek-total');
        this.stats_panel            = this.shadowRoot.querySelector('#video-stats');
        this.stage_el               = this.shadowRoot.querySelector('.stage');
        this.track_progress         = this.shadowRoot.querySelector('#track-progress');
        this.protocol_select        = this.shadowRoot.querySelector('#protocol');
        this.convert_link           = this.shadowRoot.querySelector('#convert-link');
        this.stop                  = this.shadowRoot.querySelector('#stop');
        this.back                  = this.shadowRoot.querySelector('#back');
        this.back_step             = this.shadowRoot.querySelector('#back-step');
        this.play                  = this.shadowRoot.querySelector('#play');
        this.forward_step          = this.shadowRoot.querySelector('#forward-step');
        this.forward               = this.shadowRoot.querySelector('#forward');

        this.source_field_img      = this.shadowRoot.querySelector('#player-source-img');
        this.stop_img              = this.shadowRoot.querySelector('#stop-img');
        this.back_img              = this.shadowRoot.querySelector('#back-img');
        this.back_step_img         = this.shadowRoot.querySelector('#back-step-img');
        this.play_img              = this.shadowRoot.querySelector('#play-img');
        this.forward_step_img      = this.shadowRoot.querySelector('#forward-step-img');
        this.forward_img           = this.shadowRoot.querySelector('#forward-img');
        this.source_field_img.src  = new URL('./resources/images/open-file.svg', import.meta.url);
        this.stop_img.src          = new URL('./resources/images/stop.svg', import.meta.url);
        this.back_img.src          = new URL('./resources/images/back.svg', import.meta.url);
        this.back_step_img.src     = new URL('./resources/images/back-step.svg', import.meta.url);
        this.play_img.src          = new URL('./resources/images/play.svg', import.meta.url);
        this.forward_step_img.src  = new URL('./resources/images/forward-step.svg', import.meta.url);
        this.forward_img.src       = new URL('./resources/images/forward.svg', import.meta.url);


        this.video.addEventListener('ended', () => this.on_video_ended());
        this.play.addEventListener('click', () => this.on_play());
        this.stop.addEventListener('click', () => this.on_stop());
        this.back.addEventListener('click', () => this.on_back());
        this.forward.addEventListener('click', () => this.on_forward());
        this.back_step.addEventListener('click', () => this.on_back_step());
        this.forward_step.addEventListener('click', () => this.on_forward_step());
        this.source_button.addEventListener('click', () => this.on_open_file());
        this.file_input.addEventListener('change', () => this.on_file_selected());
        this.frag_start_btn.addEventListener('click', () => this.on_frag_start());
        this.resolution_select.addEventListener('change', () => this.on_resolution_changed());
        this.session_new_btn.addEventListener('click', () => this.on_session_new());
        this.session_delete_btn.addEventListener('click', () => this.on_session_delete());
        this.session_cancel_btn.addEventListener('click', () => this.on_session_cancel());
        this.session_select.addEventListener('change', () => this.on_session_changed());
        this.refresh_sessions();

        this.in_device.addEventListener('change', () => this.on_input_device_changed());
        this.in_codec.addEventListener('change', () => this.populate_input_resolutions());
        this.in_resolution.addEventListener('change', () => this.populate_input_fps());
        [this.out_codec, this.out_resolution, this.out_fps, this.out_bitrate, this.protocol_select]
            .forEach(el => el && el.addEventListener('change', () => this.update_output_note()));
        this.refresh_devices();
        this.video.addEventListener('timeupdate', () => { this.update_progress_fill(); this.update_seek(); });
        this.video.addEventListener('loadedmetadata', () =>
        {
            // Quadro passa a usar a proporcao real do video (sem tarjas): a barra
            // sobreposta cola no rodape da imagem.
            if (this.stage_el && this.video.videoWidth > 0) this.stage_el.classList.add('has-video');
            this.update_progress_fill();
            this.update_seek();
        });
        this.video.addEventListener('durationchange', () => this.update_seek());

        // Barra do player: clique ou arraste (pointer) para navegar.
        this.seek_bar.addEventListener('pointerdown', (e) =>
        {
            this._seeking = true;
            try { this.seek_bar.setPointerCapture(e.pointerId); } catch {}
            this.seek_to_event(e);
        });
        this.seek_bar.addEventListener('pointermove', (e) => { if (this._seeking) this.seek_to_event(e); });
        const end_seek = (e) =>
        {
            this._seeking = false;
            try { this.seek_bar.releasePointerCapture(e.pointerId); } catch {}
        };
        this.seek_bar.addEventListener('pointerup', end_seek);
        this.seek_bar.addEventListener('pointercancel', end_seek);

        this.progress_bar.addEventListener('mousemove', (e) => this.on_progress_hover(e));
        this.progress_bar.addEventListener('mouseleave', () => { this.progress_tooltip.hidden = true; });
        this.progress_bar.addEventListener('click', (e) => this.on_progress_click(e));

        window.addEventListener('keydown', (e) =>
        {
            // Nao sequestra a tecla enquanto o foco esta num campo: o 'f' de um nome de
            // arquivo digitado no formulario nao pode jogar a pagina em tela cheia.
            if (this.digitando(e)) return;

            if (e.code === 'Space')
            {
                e.preventDefault();
                this.on_play();
            }
            else if (e.key === 'f' || e.key === 'F')
            {
                e.preventDefault();
                this.tela_cheia_alternar();
            }
            else if (e.key === 'm' || e.key === 'M')
            {
                e.preventDefault();
                this.mudo_alternar();
            }
            else if (e.key === 'ArrowUp' || e.key === 'ArrowDown')
            {
                e.preventDefault();
                this.volume_passo(e.key === 'ArrowUp' ? 0.05 : -0.05);
            }
        });

        if (this.mute_btn)      this.mute_btn.addEventListener('click', () => this.mudo_alternar());
        if (this.volume_slider) this.volume_slider.addEventListener('input',
                                    () => this.volume_aplicar(this.volume_slider.value / 100, false));

        // O estado vem do ELEMENTO, nao do controle: volume mudado por outro caminho
        // (teclado do sistema, script, restauracao) tem de chegar na barra do mesmo jeito.
        this.video.addEventListener('volumechange', () => this.volume_sincronizar());

        this.volume_restaurar();

        // Duplo clique no video: o gesto que todo player tem.
        this.video.addEventListener('dblclick', (e) => { e.preventDefault(); this.tela_cheia_alternar(); });
        if (this.fullscreen_btn) this.fullscreen_btn.addEventListener('click', () => this.tela_cheia_alternar());

        // O estado tem de vir do EVENTO, e nao do clique: o Esc e o botao do proprio
        // navegador saem de tela cheia sem passar por aqui, e o icone ficaria mentindo.
        this._on_fs_change = () => this.tela_cheia_sincronizar();
        document.addEventListener('fullscreenchange', this._on_fs_change);
        document.addEventListener('webkitfullscreenchange', this._on_fs_change);

        this.video.addEventListener('play',  () => this.update_player_control(PlayerControlOptions.PLAY));
        this.video.addEventListener('pause', () => this.update_player_control(PlayerControlOptions.PAUSE));
        this.video.addEventListener('ended', () => this.update_player_control(PlayerControlOptions.STOP));

        // FLUXO DO VIDEO: clique fora fecha o painel; clique no video alterna (reabre).
        this._on_doc_click = (event) => this.on_document_click(event);
        document.addEventListener('click', this._on_doc_click);
        //...
    }

    on_document_click(event)
    {
        if (!this.stats_panel) return;
        const path = event.composedPath();
        if (path.includes(this.stats_panel)) return;            // dentro do painel: mantem
        if (this.seek_row && path.includes(this.seek_row)) return; // barra do player: nao alterna

        if (path.includes(this.video) || path.includes(this.stage_el))
        {
            // Clique no video: alterna a visibilidade do painel.
            this.stats_dismissed = !this.stats_panel.hidden;
            this.stats_panel.hidden = !this.stats_panel.hidden;
            return;
        }
        if (!this.stats_panel.hidden)                           // clique fora: fecha
        {
            this.stats_panel.hidden = true;
            this.stats_dismissed = true;
        }
    }


    on_play()
    {
        if (this.video.paused)
        {
            if (!this.video.src)
            {
                this.video.src = this.VIDEO_URL_DEMO;
                this.video.load();
            }

            this.video.play().then(() => this.update_player_control(PlayerControlOptions.PLAY)).catch(err =>
            {
                console.warn('Falha ao reproduzir:', err);
            });
        }
        else
        {
            this.video.pause();
            this.update_player_control(PlayerControlOptions.PAUSE);
        }
    }

    on_stop()
    {

    }

    on_forward()
    {

    }

    on_back()
    {

    }

    on_forward_step()
    {

    }

    on_back_step()
    {

    }

    on_open_file()
    {
        const dev = this.current_device();
        if (!dev || dev.kind !== 'file') return;
        this.file_input.click();
    }

    // Fonte de captura (camera): entrada ao vivo, sem arquivo e sem escada de resolucoes.
    is_camera()
    {
        const dev = this.current_device();
        return !!dev && dev.kind !== 'file';
    }

    // O botao de abrir arquivo so vale com Fonte = Arquivo, com sessao e sem envio em curso.
    update_source_button()
    {
        const dev = this.current_device();
        const is_file = !!dev && dev.kind === 'file';
        this.source_button.disabled = !is_file || !this.session_id() || !!this._uploading;
        if (!this.frag_start_btn) return;
        // Camera: basta a sessao (nao ha upload). Arquivo: precisa do plano ja lido.
        this.frag_start_btn.disabled = this.is_camera()
            ? (!this.session_id() || !!this._uploading)
            : (!this._plan || !this.session_id() || !!this._uploading);
    }

    // ---- camera ao vivo -----------------------------------------------------
    // A resolucao de SAIDA do ao vivo sai da barra do player: e' uma escolha de
    // reproducao, nao de configuracao -- trocar reabre camera e encoder.

    codec_name(slug)
    {
        return ({ h264: 'H.264', hevc: 'H.265', h265: 'H.265', vp9: 'VP9', av1: 'AV1' })[(slug || '').toLowerCase()]
               || (slug || '').toUpperCase();
    }

    // "LARGURAxALTURA" do modo de captura escolhido na ENTRADA.
    input_resolution_value()
    {
        const st  = this.current_input_stream();
        const res = st ? (st.resolutions || [])[parseInt(this.in_resolution.value, 10)] : null;
        return res ? `${res.width}x${res.height}` : '';
    }

    populate_live_resolutions()
    {
        const st  = this.current_input_stream();
        const i   = parseInt(this.in_resolution.value, 10);
        const res = st ? (st.resolutions || [])[i] : null;
        const cw = res ? res.width : 1280, ch = res ? res.height : 720;

        const previous = this.resolution_select.value;
        this.resolution_select.replaceChildren();
        [ch, 1080, 720, 480, 360, 240, 144].forEach((h) =>
        {
            if (h > ch) return;                                   // nao aumenta o que a camera da
            const w = h === ch ? cw : Math.round(cw * h / ch / 2) * 2;
            const value = `${w}x${h}`;
            if ([...this.resolution_select.options].some(o => o.value === value)) return;
            const opt = document.createElement('option');
            opt.value = value;
            opt.textContent = `${h}p (${value})` + (h === ch ? ' — da camera' : '');
            this.resolution_select.appendChild(opt);
        });
        this.resolution_select.disabled = false;
        const values = [...this.resolution_select.options].map(o => o.value);
        this.resolution_select.value = values.includes(previous) ? previous : `${cw}x${ch}`;
    }

    // Liga/desliga a transmissao. O mesmo botao, como no arquivo.
    async on_live_toggle()
    {
        const id = this.session_id();
        if (!id) return;

        if (this._live_on) { await this.live_stop(); return; }

        this.set_uploading(true);
        this.status_field.hidden = false;
        this.status_field.textContent = 'Abrindo a camera...';
        try
        {
            await this.save_io_config();   // a resolucao de saida vai junto (barra do player)
            const data = await fetch(`/api/live/start/${encodeURIComponent(id)}`, { cache: 'no-store' })
                               .then(r => r.json());
            if (!data.ok) throw new Error(data.error || 'falha ao iniciar');

            this._live_on = true;
            this.frag_start_btn.textContent = 'Parar';
            this.live_watch(id);

            // So entra no player quando o PRIMEIRO segmento existir: com a playlist ainda
            // vazia o hls.js consome as tentativas dele e desiste antes de a camera render.
            this.status_field.textContent = 'Camera aberta. Aguardando o primeiro segmento...';
            for (let i = 0; i < 30 && this._live_on; i++)
            {
                const s = await fetch(`/api/live/status/${encodeURIComponent(id)}`, { cache: 'no-store' }).then(r => r.json());
                if (s.segments > 0) break;
                if (s.error) throw new Error(s.error);
                await new Promise(done => setTimeout(done, 500));
            }
            if (this._live_on) this.open_live(data.playlist);
        }
        catch (error)
        {
            this.status_field.textContent = 'Falha ao transmitir: ' + error.message;
            this._live_on = false;
        }
        finally { this.set_uploading(false); }
    }

    async live_stop()
    {
        const id = this.session_id();
        this._live_on = false;
        if (this._live_timer) { clearInterval(this._live_timer); this._live_timer = null; }
        if (this.hls) { this.hls.destroy(); this.hls = null; }
        if (this.frag_start_btn) this.frag_start_btn.textContent = 'Transmitir';
        try { await fetch(`/api/live/stop/${encodeURIComponent(id)}`, { cache: 'no-store' }); } catch {}
        this.status_field.textContent = 'Transmissao encerrada.';
    }

    // Playlist ao vivo: janela deslizante, entao o player tem de seguir a borda
    // (liveDurationInfinity) em vez de tratar como VOD com duracao fixa.
    open_live(playlist)
    {
        if (this.hls) { this.hls.destroy(); this.hls = null; }
        if (!window.Hls?.isSupported())
        {
            this.status_field.textContent = 'Ao vivo precisa do hls.js (coloque web/lib/hls.min.js).';
            return;
        }
        // Ao vivo: entrar perto da BORDA (nao no inicio da janela) e acelerar de leve para
        // recuperar atraso -- sem isso, uma pausa do navegador (aba oculta) deixa a
        // reproducao dezenas de segundos atras e ela nunca mais alcanca.
        this.hls = new window.Hls({
            liveDurationInfinity: true,
            lowLatencyMode: false,
            liveSyncDurationCount: 2,
            maxLiveSyncPlaybackRate: 1.5
        });
        this.hls.loadSource(`${playlist}?v=${Date.now()}`);
        this.hls.attachMedia(this.video);
        this.video.play().catch(() => {});
    }

    // Enquanto transmite, mostra quem esta codificando e quanto ha na janela.
    live_watch(id)
    {
        if (this._live_timer) clearInterval(this._live_timer);
        this._live_timer = setInterval(async () =>
        {
            if (!this._live_on) return;
            try
            {
                const s = await fetch(`/api/live/status/${encodeURIComponent(id)}`, { cache: 'no-store' }).then(r => r.json());
                if (s.error) { this.status_field.textContent = 'Falha na transmissao: ' + s.error; await this.live_stop(); return; }
                if (!s.live)  { await this.live_stop(); return; }
                this.status_field.textContent =
                    `Ao vivo: ${s.width}x${s.height} @ ${s.fps} fps — ${s.segments} segmentos na janela` +
                    (s.encoder ? ` — ${s.encoder}` : '');
                this.update_track_stats(s.width, s.height, 0);
            }
            catch { /* uma falha de status nao derruba a transmissao */ }
        }, 2000);
    }

    // Formato "do arquivo" nao e um protocolo: o container sai do codec. VP9/AV1 so
    // existem em WebM (DASH); o resto vai em HLS.
    effective_protocol()
    {
        const p = this.protocol_select ? this.protocol_select.value : 'hls';
        if (p !== 'auto') return p;
        const c = this.out_codec.value;
        return c === 'vp9' ? 'dash' : c === 'av1' ? 'dash-av1' : 'hls';
    }

    async on_frag_start()
    {
        // Camera: o botao liga/desliga a transmissao ao vivo (nao ha arquivo a preparar).
        if (this.is_camera()) { await this.on_live_toggle(); return; }

        const plan = this._plan;
        if (!plan || !this.session_id()) return;

        // Formato "Arquivo unico" nao fragmenta: transcodifica (ou remuxa, no mesmo codec)
        // o video inteiro num arquivo so. Mesmo botao, mesma SAIDA.
        if (this.protocol_select && this.protocol_select.value === 'file')
        {
            await this.save_io_config();
            this.on_convert();
            return;
        }

        this.set_uploading(true);
        this._protocol = this.effective_protocol();
        await this.save_io_config();   // a fragmentacao le a SAIDA gravada na sessao
        this.manifest = plan;
        this._allReady = false;
        this._hlsMaster = false;
        this._dashActive = false;
        this.status_field.hidden = false;
        this.status_field.textContent = 'Iniciando a fragmentacao...';
        this.start_fragmentation(plan);
    }

    // SAIDA mostra os dados do arquivo nas opcoes "em branco", para ver o que sera herdado.
    update_output_labels()
    {
        // Camera: o "em branco" segue o MODO escolhido na ENTRADA. Modo comprimido sai como
        // veio (sem reencode); modo cru nao existe em HLS, entao o padrao e H.264.
        if (this.is_camera())
        {
            const st = this.current_input_stream();
            this.out_codec.options[0].textContent = (st && st.codec)
                ? `(da camera: ${this.codec_name(st.codec)}, sem reencode)`
                : '(padrao: H.264)';
            this.out_fps.options[0].textContent     = '(da camera)';
            this.out_bitrate.options[0].textContent = '(automatico)';
            this.out_resolution.options[0].textContent = '(barra do player)';
            return;
        }

        const src  = this._plan ? (this._plan.source || {}) : null;
        const auto = this.protocol_select && this.protocol_select.value === 'auto';
        const codec_name = (c) => ({ h264: 'H.264', hevc: 'H.265', h265: 'H.265', vp9: 'VP9', av1: 'AV1' })[(c || '').toLowerCase()] || (c || '?');
        const fps = src ? parseFloat(src.fps) : 0;

        this.out_codec.options[0].textContent = !auto ? '(do formato)'
            : src ? `(do arquivo: ${codec_name(src.vcodec)})` : '(do arquivo)';
        // Resolucao em branco e SEMPRE a escada adaptativa (varias resolucoes), inclusive
        // no formato "do arquivo" -- ali o que vem do arquivo e o codec.
        this.out_resolution.options[0].textContent = '(adaptativa)';
        // A resolucao do proprio arquivo entra na lista: e ela que da pista unica sem conversao.
        if (src && this._plan.width > 0)
        {
            const value = `${this._plan.width}x${this._plan.height}`;
            let own = [...this.out_resolution.options].find(o => o.value === value);
            if (!own)
            {
                own = document.createElement('option');
                own.value = value;
                this.out_resolution.add(own, this.out_resolution.options[1]);
            }
            own.textContent = `${value} (do arquivo)`;
        }
        this.out_fps.options[0].textContent = src && fps > 0
            ? `(do arquivo: ${Number(fps.toFixed(2))})` : '(da entrada)';
        this.out_bitrate.options[0].textContent = src && src.vbitrate
            ? `(do arquivo: ${this.format_bitrate(src.vbitrate)})` : '(automatico)';
    }

    set_uploading(value)
    {
        this._uploading = value;
        this.update_source_button();
    }

    // ---- Entrada / Saida ----------------------------------------------------
    // A arvore da entrada vem inteira de /api/media/devices (device -> stream ->
    // resolucao -> fps). As opcoes NAO sao independentes: trocar o codec troca as
    // resolucoes possiveis, e trocar a resolucao troca os fps. Por isso cada combo
    // repopula o seguinte, em vez de listas fixas.

    async refresh_devices()
    {
        const session = this.session_id();
        const url = session ? `/api/media/devices/${encodeURIComponent(session)}` : '/api/media/devices';
        try
        {
            const data = await fetch(url, { cache: 'no-store' }).then(r => r.json());
            this._devices = data.devices || [];
            this._captureAvailable = data.captureAvailable !== false;
        }
        catch (error)
        {
            this._devices = [];
            this.in_note.textContent = 'Falha ao listar dispositivos: ' + error.message;
            return;
        }

        // "Arquivo" existe SEMPRE: o servidor so lista o arquivo depois do envio (source.mp4
        // da sessao); antes disso a opcao e local, e serve para liberar o botao de abrir.
        if (!this._devices.some(d => d.kind === 'file'))
            this._devices.unshift({ id: '', name: 'selecionar arquivo...', kind: 'file', streams: [] });

        const previous = this.in_device.value;
        this.in_device.innerHTML = '';

        this._devices.forEach((d, i) =>
        {
            const opt = document.createElement('option');
            opt.value = String(i);
            opt.textContent = (d.kind === 'file' ? 'Arquivo: ' : 'Camera: ') + d.name;
            this.in_device.appendChild(opt);
        });
        if (!this._devices.some(d => d.kind !== 'file'))
        {
            const opt = document.createElement('option');
            opt.value = '';
            opt.disabled = true;
            opt.textContent = this._captureAvailable ? 'Nenhuma camera encontrada' : 'Captura indisponivel neste sistema';
            this.in_device.appendChild(opt);
        }
        this.in_device.value = [...this.in_device.options].some(o => o.value === previous && !o.disabled) ? previous : '0';

        this.on_input_device_changed();
    }

    current_device()
    {
        const i = parseInt(this.in_device.value, 10);
        return (this._devices || [])[i] || null;
    }

    on_input_device_changed()
    {
        const dev = this.current_device();
        const streams = dev ? (dev.streams || []) : [];

        this.in_codec.innerHTML = '';
        streams.forEach((st, i) =>
        {
            const opt = document.createElement('option');
            opt.value = String(i);
            opt.textContent = st.label + (st.supported ? '' : ' (nao suportado)');
            opt.disabled = !st.supported;
            this.in_codec.appendChild(opt);
        });

        const first = streams.findIndex(st => st.supported);
        if (first >= 0) this.in_codec.value = String(first);

        // Arquivo nao tem escolha: resolucao/fps/codec sao os do proprio video.
        // Camera tambem nao: o formato de captura fica com o servidor; o que o usuario
        // escolhe e' a SAIDA (codec/fps/bitrate) e a resolucao na barra do player.
        const is_file = dev && dev.kind === 'file';
        this.in_codec.disabled = is_file || streams.length <= 1;
        const file_name = this._file_name ? ` Selecionado: ${this._file_name}.` : '';
        this.in_note.textContent = !dev ? ''
            : is_file ? 'Arquivo: clique no botao ao lado da fonte para abrir. Resolucao, FPS e codec vem do proprio video.' + file_name
                      : 'Camera: formato de captura definido pelo servidor. A resolucao de SAIDA fica na barra do player.';
        this.update_source_button();

        this.populate_input_resolutions();
    }

    current_input_stream()
    {
        const dev = this.current_device();
        if (!dev) return null;
        const i = parseInt(this.in_codec.value, 10);
        return (dev.streams || [])[i] || null;
    }

    populate_input_resolutions()
    {
        const st = this.current_input_stream();
        const list = st ? (st.resolutions || []) : [];
        this.in_resolution.innerHTML = '';
        list.forEach((r, i) =>
        {
            const opt = document.createElement('option');
            opt.value = String(i);
            opt.textContent = `${r.width}x${r.height}`;
            this.in_resolution.appendChild(opt);
        });
        const dev = this.current_device();
        this.in_resolution.disabled = (dev && dev.kind === 'file') || list.length <= 1;
        this.populate_input_fps();
    }

    populate_input_fps()
    {
        const st = this.current_input_stream();
        const i  = parseInt(this.in_resolution.value, 10);
        const res = st ? (st.resolutions || [])[i] : null;
        const list = res ? (res.fps || []) : [];

        this.in_fps.innerHTML = '';
        list.forEach((f) =>
        {
            const opt = document.createElement('option');
            opt.value = String(f);
            opt.textContent = Number(f).toFixed(Number(f) % 1 ? 2 : 0);
            this.in_fps.appendChild(opt);
        });
        const dev = this.current_device();
        this.in_fps.disabled = (dev && dev.kind === 'file') || list.length <= 1;
        if (this.is_camera()) this.populate_live_resolutions();   // barra do player = resolucao de saida
        this.update_output_note();
    }

    // Reflete a politica de bypass: nada escolhido = sem conversao.
    update_output_note()
    {
        if (!this.out_note) return;
        this.update_output_labels();
        const nada = !this.out_codec.value && !this.out_resolution.value &&
                     !this.out_fps.value && !this.out_bitrate.value;
        const auto = this.protocol_select && this.protocol_select.value === 'auto';
        const hevc = this._plan && /^(hevc|h265)$/i.test((this._plan.source || {}).vcodec || '');
        // Camera: ao vivo em HLS, pista unica, resolucao pela barra do player. Sobra para a
        // SAIDA o que faz sentido em tempo real: codec, FPS e bitrate.
        if (this.is_camera())
        {
            this.out_resolution.disabled = true;
            this.out_codec.disabled = false; this.out_fps.disabled = false; this.out_bitrate.disabled = false;
            if (this.protocol_select) this.protocol_select.disabled = true;
            if (this.frag_start_btn) this.frag_start_btn.textContent = this._live_on ? 'Parar' : 'Transmitir';

            const st = this.current_input_stream();
            const encoded = !!(st && st.codec);

            // Modo COMPRIMIDO: o codec de saida e' o da camera e ponto -- trocar obrigaria a
            // decodificar e recodificar justamente o que ja vem pronto. Modo CRU e' o unico
            // que precisa de escolha (o cru nao existe em HLS).
            this.out_codec.disabled = encoded;
            if (encoded) this.out_codec.value = '';

            const mexeu = !!this.out_fps.value || !!this.out_bitrate.value;
            const escala = !!this.resolution_select.value &&
                           this.resolution_select.value !== this.input_resolution_value();
            this.out_note.textContent = !encoded
                ? `Modo cru da camera (${(st && st.label) || '?'}): sera codificado em ${this.out_codec.value ? this.out_codec.options[this.out_codec.selectedIndex].textContent : 'H.264'}. HLS em memoria, nada e gravado.`
                : mexeu || escala
                    ? `Conversao ativa (${escala ? 'resolucao menor que a da camera' : 'FPS/bitrate alterados'}): o gateway decodifica e recodifica em ${this.codec_name(st.codec)}.`
                    : `Modo comprimido (${this.codec_name(st.codec)}): o bitstream da camera vai direto, sem passar pelo gateway.`;
            if (this.frag_hint)
                this.frag_hint.textContent = this._live_on
                    ? 'Transmitindo. "Parar" encerra e libera a memoria.'
                    : `Pronto para transmitir em ${this.resolution_select.value || '...'}.`;
            return;
        }
        if (this.protocol_select) this.protocol_select.disabled = false;

        // Arquivo unico: nao ha escada nem segmentos; resolucao/FPS/bitrate nao se aplicam
        // (o servidor transcodifica o video inteiro no codec escolhido, ou so remuxa).
        const to_file = this.protocol_select && this.protocol_select.value === 'file';
        [this.out_resolution, this.out_fps, this.out_bitrate].forEach(el => { el.disabled = to_file; });
        if (this.frag_start_btn) this.frag_start_btn.textContent = to_file ? 'Converter' : 'Fragmentar';

        const one_track = !!this.out_resolution.value;
        if (to_file)
            this.out_note.textContent = this.out_codec.value
                ? `Arquivo unico no codec ${this.out_codec.options[this.out_codec.selectedIndex].textContent}: o video inteiro e reencodado, sem HLS/DASH.`
                : 'Arquivo unico no codec do proprio arquivo: so remuxa, sem reencode.';
        else if (auto)
            this.out_note.textContent = hevc
                ? 'Do arquivo: H.265 nao tem saida HLS (so avc1); sera convertido para H.264.'
                : one_track
                    ? 'Resolucao fixa = pista unica. Igual a do arquivo e sem outras mudancas: sem conversao (bypass), o caminho mais rapido.'
                    : 'Do arquivo: mantem o codec do arquivo (sem conversao de codec) e gera a escada de resolucoes.';
        else
            this.out_note.textContent = nada
                ? 'Formato fixo: escada adaptativa de resolucoes no codec do formato.'
                : 'Conversao ativa. Campos em branco herdam a entrada.';
        if (this.frag_hint)
            this.frag_hint.textContent = !this._plan ? 'Selecione um arquivo na ENTRADA.'
                : `Pronto: ${this._file_name || 'arquivo'} (${to_file ? 'arquivo unico' : this.effective_protocol().startsWith('dash') ? 'DASH' : 'HLS'}).`;
    }

    // Grava Entrada/Saida na sessao. A fragmentacao le dali -- assim a escolha sobrevive
    // a um reload da pagina e fica registrada no session.json junto do resultado.
    async save_io_config()
    {
        const id = this.session_id();
        if (!id) return;

        const dev = this.current_device();
        const st  = this.current_input_stream();
        const ri  = parseInt(this.in_resolution.value, 10);
        const res = st ? (st.resolutions || [])[ri] : null;

        // Na camera a resolucao de saida vem da barra do player; no arquivo, da SAIDA.
        const wh = ((this.is_camera() ? this.resolution_select.value : this.out_resolution.value) || '').split('x');

        const body = {
            input: {
                kind:   dev ? dev.kind : 'file',
                device: dev ? dev.id : '',
                // Comprimido: o slug do codec. Cru: o pixfmt (o servidor abre a camera
                // exatamente nesse modo, e e' o que decide reencodar ou nao).
                codec:  st ? (st.codec || st.pixfmt || st.label) : '',
                width:  res ? res.width : 0,
                height: res ? res.height : 0,
                fps:    parseFloat(this.in_fps.value || '0') || 0
            },
            output: {
                protocol: this.protocol_select ? this.protocol_select.value : '',
                codec:    this.out_codec.value || '',
                width:    wh.length === 2 ? parseInt(wh[0], 10) : 0,
                height:   wh.length === 2 ? parseInt(wh[1], 10) : 0,
                fps:      parseFloat(this.out_fps.value || '0') || 0,
                bitrate:  parseInt(this.out_bitrate.value || '0', 10) || 0
            }
        };

        try
        {
            await fetch(`/api/session/config/${encodeURIComponent(id)}`,
            {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(body)
            });
        }
        catch (error) { /* a fragmentacao ainda funciona com os defaults */ }
    }

    // ---- sessoes de fragmentacao -------------------------------------------
    // Cada sessao e uma subpasta com os videos gerados e um session.json de controle.
    // O id da sessao substitui o antigo nome de pasta digitado a mao: e ele que vai no
    // X-Hls-Folder e nas rotas de fragmentacao.

    session_id()
    {
        return this.session_select ? this.session_select.value : '';
    }

    // Recarrega a lista, preservando a selecao atual quando ela ainda existir.
    async refresh_sessions(prefer_id)
    {
        const wanted = prefer_id || this.session_id();
        let list = [];
        try
        {
            const response = await fetch('/api/session/list', { cache: 'no-store' });
            const data = await response.json();
            list = data.sessions || [];
        }
        catch (error)
        {
            this.set_session_info('Falha ao listar sessoes: ' + error.message);
            return;
        }

        this._sessions = list;
        this.session_select.innerHTML = '';

        if (list.length === 0)
        {
            const opt = document.createElement('option');
            opt.value = '';
            opt.textContent = 'Nenhuma sessao - clique em "Nova sessao"';
            this.session_select.appendChild(opt);
        }
        else
        {
            // Mais recentes primeiro: o id comeca com a data/hora, entao ordenar por id basta.
            list.sort((a, b) => (b.id || '').localeCompare(a.id || ''));
            list.forEach((item) =>
            {
                const opt = document.createElement('option');
                opt.value = item.id;
                opt.textContent = item.name && item.name !== item.id ? `${item.name} (${item.id})` : item.id;
                this.session_select.appendChild(opt);
            });
            this.session_select.value = list.some(i => i.id === wanted) ? wanted : list[0].id;
        }

        this.on_session_changed();
    }

    // Recarrega a lista SO depois que o job realmente encerrou no servidor.
    async refresh_sessions_settled(id)
    {
        for (let i = 0; i < 20 && id; i++)
        {
            try
            {
                const response = await fetch(`/api/session/get/${encodeURIComponent(id)}`, { cache: 'no-store' });
                const data = await response.json();
                if (data.running === false || data.running === 'false') break;
            }
            catch { break; }
            await new Promise(done => setTimeout(done, 400));
        }
        return this.refresh_sessions();
    }

    current_session()
    {
        const id = this.session_id();
        return (this._sessions || []).find(i => i.id === id) || null;
    }

    set_session_info(text, state)
    {
        if (!this.session_info) return;
        this.session_info.innerHTML = '';
        this.session_info.append(text);
        if (state)
        {
            const chip = document.createElement('span');
            chip.className = `session-state state-${state}`;
            chip.textContent = state;
            this.session_info.appendChild(chip);
        }
    }

    on_session_changed()
    {
        const item = this.current_session();
        const has  = !!this.session_id();

        // Sem sessao nao ha onde gravar: o envio de video fica bloqueado.
        this.update_source_button();
        this.session_delete_btn.disabled = !has;

        if (!item)
        {
            this.set_session_info(has ? 'Sessao selecionada.' : 'Nenhuma sessao. Crie uma para comecar.');
            return;
        }

        const running = item.running === true || item.running === 'true';
        this.session_cancel_btn.hidden = !running;
        const created = item.created ? ` - criada em ${item.created}` : '';
        this.set_session_info(`${item.id}${created}`, item.state || 'idle');
        if (this._devices_for !== item.id) { this._devices_for = item.id; this.refresh_devices(); }
    }

    async on_session_new()
    {
        const name = this.session_name_field ? this.session_name_field.value.trim() : '';

        this.session_new_btn.disabled = true;
        try
        {
            const response = await fetch('/api/session/create',
            {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ name: name })
            });
            const data = await response.json();
            if (!data.id) throw new Error(data.error || 'resposta sem id');
            if (this.session_name_field) this.session_name_field.value = '';
            await this.refresh_sessions(data.id);
            this.reset_for_new_session();
        }
        catch (error)
        {
            this.set_session_info('Falha ao criar sessao: ' + error.message);
        }
        finally
        {
            this.session_new_btn.disabled = false;
        }
    }

    async on_session_delete()
    {
        const id = this.session_id();
        if (!id) return;

        // Primeiro clique arma; o segundo confirma. Evita o confirm() do navegador e
        // ainda assim nao apaga a sessao por um clique acidental.
        if (this._delete_armed !== id)
        {
            this._delete_armed = id;
            this.session_delete_btn.textContent = 'Confirmar?';
            clearTimeout(this._delete_timer);
            this._delete_timer = setTimeout(() =>
            {
                this._delete_armed = null;
                this.session_delete_btn.textContent = 'Excluir';
            }, 4000);
            return;
        }

        clearTimeout(this._delete_timer);
        this._delete_armed = null;
        this.session_delete_btn.textContent = 'Excluir';
        this.session_delete_btn.disabled = true;
        try
        {
            const response = await fetch(`/api/session/delete/${encodeURIComponent(id)}`, { cache: 'no-store' });
            const data = await response.json();
            if (!data.ok) throw new Error(data.error || 'falha ao remover');
            this.reset_for_new_session();
            await this.refresh_sessions();
        }
        catch (error)
        {
            this.set_session_info('Falha ao excluir: ' + error.message);
        }
        finally
        {
            this.session_delete_btn.disabled = false;
        }
    }

    async on_session_cancel()
    {
        const id = this.session_id();
        if (!id) return;

        this.session_cancel_btn.disabled = true;
        this.status_field.hidden = false;
        this.status_field.textContent = 'Cancelando... (a saida nao sera publicada)';
        try
        {
            await fetch(`/api/session/cancel/${encodeURIComponent(id)}`, { cache: 'no-store' });
        }
        catch (error)
        {
            this.status_field.textContent = 'Falha ao cancelar: ' + error.message;
        }
        finally
        {
            this.session_cancel_btn.disabled = false;
        }
    }

    // Zera o que pertence a sessao anterior. Sem isto, trocar de sessao deixava na tela o
    // plano de pistas, o progresso e o player da sessao antiga.
    reset_for_new_session()
    {
        if (this._live_on) this.live_stop();   // a transmissao pertence a sessao que saiu
        if (this.event_source) { this.event_source.close(); this.event_source = null; }
        this.manifest      = null;
        this._plan         = null;
        this._file_name    = null;
        this._allReady     = false;
        this._hlsMaster    = false;
        this._dashActive   = false;
        this.status_field.hidden = true;
        this.status_field.textContent = '';
        if (this.frag_time)     this.frag_time.hidden = true;
        this.frag_head_parar();
        if (this.frag_head)     this.frag_head.hidden = true;
        if (this.progress_wrap) this.progress_wrap.hidden = true;
        if (this.track_progress) this.track_progress.innerHTML = '';
        if (this.convert_link)  this.convert_link.hidden = true;
        if (this.session_cancel_btn) this.session_cancel_btn.hidden = true;
        if (this.resolution_select)
        {
            this.resolution_select.innerHTML = '<option value="">Envie um video...</option>';
            this.resolution_select.disabled = true;
        }
        this.update_output_note();
        this.update_source_button();
    }

    async on_file_selected()
    {
        const file = this.file_input.files?.[0];
        if (!file) return;
        this._file_name = file.name;
        this.on_input_device_changed();   // mostra o nome do arquivo na nota da entrada

        // Preview imediato do VIDEO SELECIONADO (nao a fonte de teste fixa). Quando a
        // fragmentacao terminar, open_hls troca para o HLS adaptativo.
        if (this.object_url) URL.revokeObjectURL(this.object_url);
        this.object_url = URL.createObjectURL(file);
        this.video.src = this.object_url;
        this.video.load();

        const folder = this.session_id();
        if (!folder)
        {
            this.status_field.hidden = false;
            this.status_field.textContent = 'Crie ou selecione uma sessao antes de enviar o video.';
            return;
        }

        this.status_field.hidden = false;
        this.status_field.textContent = 'Enviando o arquivo e lendo os metadados...';
        this._plan = null;   // plano do arquivo anterior nao vale mais
        this.set_uploading(true);
        await this.save_io_config();   // a fragmentacao le a config da sessao

        // Passo 1: upload + probe. Retorna o plano de pistas (sem fragmentar).
        try
        {
            const response = await fetch('/api/video/prepare',
            {
                method: 'POST',
                headers: { 'Content-Type': 'application/octet-stream', 'X-Hls-Folder': folder },
                body: file
            });
            const text = await response.text();
            // O servidor sempre responde 200; erros vem como texto puro (nao-JSON).
            const trimmed = text.trimStart();
            if (!trimmed.startsWith('{')) throw new Error(text || `HTTP ${response.status}`);
            const plan = JSON.parse(trimmed);

            // Para aqui: selecionar o arquivo NAO fragmenta. O plano fica guardado e o
            // botao Fragmentar dispara o passo 2 com a SAIDA que estiver escolhida entao.
            this._plan = plan;
            this.stats_dismissed = false;
            if (this.convert_link) this.convert_link.hidden = true;
            this.update_stats(plan);

            // SAIDA volta para "do arquivo" com os dados do arquivo recem-lido.
            if (this.protocol_select) this.protocol_select.value = 'auto';
            [this.out_codec, this.out_resolution, this.out_fps, this.out_bitrate].forEach(el => el.value = '');

            // ENTRADA: o servidor agora lista o arquivo da sessao com codec/resolucao/fps
            // lidos do proprio MP4; os campos mostram esses dados, desativados.
            await this.refresh_devices();
            this.set_uploading(false);
            this.update_output_note();
            this.status_field.textContent = 'Arquivo enviado e metadados lidos. Ajuste a SAIDA, se quiser, e clique em Fragmentar.';
        }
        catch (error)
        {
            this.status_field.textContent = `Falha ao preparar HLS: ${error.message}`;
            this.set_uploading(false);
            console.error(error);
        }
    }

    start_fragmentation(plan)
    {
        if (this.event_source) this.event_source.close();
        this.reset_track_progress(plan.tracks ? plan.tracks.length : 0);   // checklist montado pelos eventos SSE

        // HLS (H.264) ou DASH (VP9/AV1) conforme o formato escolhido. O codec vai no
        // caminho do endpoint DASH (EventSource nao permite header).
        let stream_url = plan.stream;
        if (this._protocol.indexOf('dash') === 0 && plan.dashStream)
        {
            const codec = this._protocol === 'dash-av1' ? 'av1' : 'vp9';
            stream_url = plan.dashStream + '/' + codec;
        }
        const source = new EventSource(stream_url);
        this.event_source = source;

        source.onmessage = (message) =>
        {
            let ev;
            try { ev = JSON.parse(message.data); }
            catch { return; }

            if (ev.type === 'start')
            {
                this.reset_track_progress(ev.total || 0);
                this.frag_head_iniciar();
                if (this.frag_time) { this.frag_time.hidden = true; this.frag_time.textContent = ''; }
                if (this.session_cancel_btn) this.session_cancel_btn.hidden = false;
                this.refresh_sessions();   // o chip sai de "idle" para "running"
            }
            else if (ev.type === 'track-start')
            {
                this.ensure_track_row(ev.name, `${ev.height}p`);   // cria a linha sob demanda
                this.set_track_status(ev.name, 'active');
                // Quem codifica esta pista (hardware/GPU/SIMD/threads): vem do seletor do servidor.
                if (ev.encoder) this.set_track_encoder(ev.name, ev.encoder);
                this.status_field.textContent = `Fragmentando ${ev.height}p (${ev.width}x${ev.height})...`;
                // FLUXO DO VIDEO acompanha a resolucao que esta sendo fragmentada agora.
                this.update_track_stats(ev.width, ev.height, ev.bandwidth);
            }
            else if (ev.type === 'track-done')
            {
                // this.manifest so e setado no 'done'; no fluxo faseado o track-done vem antes.
                const track = this.manifest?.tracks?.find(t => t.name === ev.name);
                if (track) { track.segments = ev.segments; track.playlist = ev.playlist; track.ready = true; }
                // enche a barra da pista concluida em 100% e marca "pronto"
                if (this._track_rows && this._track_rows[ev.name])
                {
                    this.frag_head_pista_pronta();
                    const row = this._track_rows[ev.name];
                    const bar = row.querySelector('.track-limit i'); if (bar) bar.style.width = '100%';
                }
                this.set_track_status(ev.name, 'done');

                const value = this.resolution_select.value;
                if (value !== 'auto' && this.manifest?.tracks)
                {
                    const chosen = this.manifest.tracks[Number(value)];
                    if (chosen && chosen.name === ev.name) this.render_progress(chosen);
                }
            }
            else if (ev.type === 'progress')
            {
                const proto = this._protocol === 'dash' ? 'DASH/VP9'
                            : this._protocol === 'dash-av1' ? 'DASH/AV1' : 'HLS';
                this.status_field.hidden = false;
                const applyRow = (row) =>
                {
                    if (!row || row.classList.contains('is-done')) return;
                    const bar = row.querySelector('.track-limit i');
                    const meta = row.querySelector('.track-meta');
                    if (bar)  bar.style.width = ev.percent + '%';
                    if (meta) meta.textContent = ev.percent + '%';
                };
                if (ev.name)
                {
                    // Progresso POR PISTA (faseado): aplica so na linha da rendition.
                    this.status_field.textContent = `Fragmentando (${proto})… ${ev.name} ${ev.percent}%`;
                    if (this._track_rows) applyRow(this._track_rows[ev.name]);
                    this.frag_head_progresso_pista(ev.percent);
                }
                else
                {
                    // Progresso GLOBAL (per-frame): aplica em todas as linhas ativas.
                    this.status_field.textContent = `Fragmentando (${proto})… ${ev.percent}%`;
                    if (this._track_rows) for (const name in this._track_rows) applyRow(this._track_rows[name]);
                    this.frag_head_progresso(ev.percent);
                }
            }
            else if (ev.type === 'done')
            {
                // Terminou de verdade: some o botao de cancelar e a lista recarrega para
                // refletir o novo estado ("done") gravado no session.json.
                if (this.session_cancel_btn) this.session_cancel_btn.hidden = true;
                this.frag_head_concluir(ev.elapsed);
                this.refresh_sessions_settled(this.session_id());
                this.manifest = ev;
                // ev.tracks sao objetos novos; remarca ready/playlist (o master vira o caminho normal,
                // mas isso mantem o fallback por pista coerente caso o master nao suba).
                (ev.tracks || []).forEach(t => { t.ready = true; this.set_track_status(t.name, 'done'); });
                this._allReady = true;                 // agora, e so agora, libera a selecao de qualidade
                const started = (ev.protocol === 'dash') ? this.open_dash(ev.playlist)
                                                          : this.open_hls(ev.playlist);
                this.populate_resolutions(ev.tracks);  // habilita o combo (Auto + resolucoes)
                const proto = ev.protocol === 'dash' ? 'DASH/VP9' : 'HLS/H.264';
                // Tempo total da fragmentacao (do servidor): "2m 03s" ou "45.2s".
                const tempo = (typeof ev.elapsed === 'number')
                    ? (ev.elapsed >= 60
                        ? `${Math.floor(ev.elapsed / 60)}m ${String(Math.round(ev.elapsed % 60)).padStart(2, '0')}s`
                        : `${ev.elapsed.toFixed(1)}s`)
                    : null;
                // So sobrescreve com "Pronto" se a reproducao iniciou; senao mantem o
                // erro que open_dash/open_hls escreveu (ex.: dash.js/hls.js nao carregou).
                if (started)
                    this.status_field.textContent =
                        `Pronto (${proto}) — ${ev.tracks.length} pistas fragmentadas${tempo ? ` em ${tempo}` : ''}. Qualidade liberada (Auto/adaptativo).`;
                // Tempo total num elemento PROPRIO (persiste; nao e sobrescrito por
                // troca de resolucao/variante como o status_field).
                if (tempo && this.frag_time)
                {
                    this.frag_time.textContent = `Fragmentacao concluida em ${tempo} (${ev.tracks.length} pistas).`;
                    this.frag_time.hidden = false;
                }
                this.set_uploading(false);
                source.close();
                this.event_source = null;
            }
            else if (ev.type === 'cancelled')
            {
                this.status_field.textContent = 'Fragmentacao interrompida. A saida NAO foi publicada.';
                this.frag_head_interromper('cancelado');
                this.set_uploading(false);
                if (this.session_cancel_btn) this.session_cancel_btn.hidden = true;
                source.close();
                this.event_source = null;
                this.refresh_sessions_settled(this.session_id());
            }
            else if (ev.type === 'error')
            {
                this.status_field.textContent = `Falha na fragmentacao: ${ev.message}`;
                this.frag_head_interromper('falhou');
                this.set_uploading(false);
                if (this.session_cancel_btn) this.session_cancel_btn.hidden = true;
                source.close();
                this.event_source = null;
                this.refresh_sessions_settled(this.session_id());
            }
        };

        source.onerror = () =>
        {
            // EventSource tenta reconectar sozinho; se ainda nao terminou, avisa.
            if (this.event_source)
            {
                this.status_field.textContent = 'Conexao SSE perdida durante a fragmentacao.';
                this.set_uploading(false);
                source.close();
                this.event_source = null;
            }
        };
    }

    open_hls(playlist)
    {
        if (this.hls) { this.hls.destroy(); this.hls = null; }
        const url = `${playlist}?v=${Date.now()}`;
        const preserve = this.video.currentTime || 0;
        const restore = () =>
        {
            if (preserve > 0) { try { this.video.currentTime = preserve; } catch {} }
            this.video.removeEventListener('loadedmetadata', restore);
        };

        // PREFERIR hls.js quando suportado (Chrome/Chromium/Edge/Firefox): so ele expoe a
        // API de troca MANUAL de nivel (currentLevel). O HLS NATIVO (canPlayType) fica como
        // fallback (Safari), onde a qualidade e controlada pelo browser e nao ha como fixar
        // a pista por JS. ORDEM IMPORTA: alguns Chromium reportam canPlayY('mpegurl')="maybe",
        // entao checar nativo primeiro roubava o caminho do hls.js -> this.hls=null -> a troca
        // de resolucao caia no ramo "ainda sendo fragmentada".
        if (window.Hls?.isSupported())
        {
            this._hlsMaster = true;
            this.video.addEventListener('loadedmetadata', restore);
            this.hls = new window.Hls({ startLevel: -1 });
            this.hls.loadSource(url);
            this.hls.attachMedia(this.video);
            // Confirma na tela qual variante HLS esta de fato tocando.
            this.hls.on(window.Hls.Events.LEVEL_SWITCHED, (_e, data) =>
            {
                const level = this.hls.levels[data.level];
                if (level)
                {
                    this.status_field.textContent =
                        `Reproduzindo variante ${level.height}p (${level.width}x${level.height}, nivel HLS ${data.level}).`;
                    this.update_track_stats(level.width, level.height, level.bitrate);
                    this.set_codec_stats('H264', 'AAC');   // saida do HLS (ffmpeg): H.264/AAC
                }
            });
            this.video.play().catch(() => {});
            return true;
        }

        if (this.video.canPlayType('application/vnd.apple.mpegurl'))
        {
            // HLS nativo (Safari): a troca de nivel e controlada pelo proprio browser.
            this._hlsMaster = true;
            this.video.addEventListener('loadedmetadata', restore);
            this.video.src = url;
            this.video.play().catch(() => {});
            return true;
        }

        // Sem HLS/MSE: nao lanca excecao silenciosa. As pistas existem no server, mas a
        // troca de resolucao depende do hls.js — deixa isso EVIDENTE na tela.
        this._hlsMaster = false;
        this.status_field.hidden = false;
        this.status_field.textContent =
            'HLS indisponivel: o hls.js nao carregou (offline/CDN bloqueado?). As pistas foram geradas no servidor, mas a troca de resolucao precisa do hls.js — coloque web/lib/hls.min.js.';
        return false;
    }

    // DASH/VP9 via dash.js. Mantem a posicao e reflete a variante no painel.
    open_dash(url)
    {
        if (this.hls) { this.hls.destroy(); this.hls = null; }
        this._hlsMaster = false;
        if (this._dash) { try { this._dash.destroy(); } catch {} this._dash = null; }

        if (!window.dashjs || !window.dashjs.MediaPlayer)
        {
            this._dashActive = false;
            this.status_field.hidden = false;
            this.status_field.textContent =
                'DASH indisponivel: o dash.js nao carregou (offline/CDN bloqueado?). Coloque web/lib/dash.all.min.js.';
            return false;
        }

        const preserve = this.video.currentTime || 0;
        const events = window.dashjs.MediaPlayer.events;
        this._dash = window.dashjs.MediaPlayer().create();
        this._dashActive = true;
        this._dash.initialize(this.video, `${url}?v=${Date.now()}`, true);

        this._dash.on(events.STREAM_INITIALIZED, () =>
        {
            if (preserve > 0) { try { this.video.currentTime = preserve; } catch {} }
        });
        // Confirma qual representation esta tocando (reflete no FLUXO DO VIDEO).
        const vlabel = (this._protocol === 'dash-av1') ? 'AV1' : 'VP9';
        this._dash.on(events.QUALITY_CHANGE_RENDERED, (e) =>
        {
            if (e.mediaType !== 'video') return;
            const list = this._dash.getBitrateInfoListFor('video') || [];
            const info = list[e.newQuality];
            if (info)
            {
                this.status_field.textContent = `Reproduzindo variante ${info.height}p (DASH, ${vlabel}).`;
                this.update_track_stats(info.width, info.height, info.bitrate);
            }
            // codec REAL de saida (so dispara quando o DASH renderiza de fato)
            this.set_codec_stats(vlabel, 'Opus');
        });
        return true;
    }

    populate_resolutions(tracks)
    {
        const previous = this.resolution_select.value;
        this.resolution_select.replaceChildren();

        // "Auto" (adaptativo) como nos players tradicionais: o ABR do HLS escolhe.
        const auto = document.createElement('option');
        auto.value = 'auto';
        auto.textContent = 'Auto (adaptativo)';
        this.resolution_select.append(auto);

        tracks.forEach((track, index) =>
        {
            const option = document.createElement('option');
            option.value = String(index);
            option.textContent = `${track.height}p (${track.width}x${track.height})`;
            this.resolution_select.append(option);
        });
        // So habilita quando TODAS as pistas estiverem fragmentadas (this._allReady).
        this.resolution_select.disabled = tracks.length === 0 || !this._allReady;

        // Mantem a escolha anterior se ainda existir; senao cai em Auto (padrao dos players).
        const values = Array.from(this.resolution_select.options).map(o => o.value);
        this.resolution_select.value = values.includes(previous) ? previous : 'auto';
        this.on_resolution_changed();
    }

    async on_resolution_changed()
    {
        // Camera: a barra do player escolhe a resolucao de SAIDA. Com a transmissao no ar,
        // trocar exige reabrir camera e encoder -- entao para e sobe de novo.
        if (this.is_camera())
        {
            this.update_output_note();
            if (this._live_on)
            {
                await this.live_stop();
                await this.on_live_toggle();
            }
            return;
        }

        const value = this.resolution_select.value;
        this.status_field.hidden = false;

        // AUTO = ABR adaptativo (igual ao "Auto" do YouTube). So faz sentido no master
        // (todas as variantes); durante a fragmentacao por pista, fica pendente.
        if (value === 'auto')
        {
            if (this._dashActive && this._dash)
            {
                this._dash.updateSettings({ streaming: { abr: { autoSwitchBitrate: { video: true } } } });
                this.status_field.textContent = 'Qualidade automatica (DASH/ABR).';
            }
            else if (this._hlsMaster && this.hls)
            {
                this.hls.currentLevel = -1;
                this.status_field.textContent = 'Qualidade automatica (adaptativa).';
            }
            else
            {
                this.status_field.textContent = 'Auto (adaptativo) fica disponivel quando a fragmentacao terminar.';
            }
            const any_track = this.manifest?.tracks?.[0];
            if (any_track) this.render_progress(any_track); // contagem de fragmentos e igual entre variantes
            return;
        }

        const index = Number(value);
        const track = this.manifest?.tracks?.[index];
        if (!track) return;

        if (this._dashActive && this._dash)
        {
            // Pin manual no DASH (dash.js): desliga o ABR e forca a representation por HEIGHT.
            // dash.js indexa a qualidade em ordem ASCENDENTE (menor->maior); o combo/tracks
            // vem em ordem descendente, entao NUNCA usar o indice cru do combo (invertia).
            this._dash.updateSettings({ streaming: { abr: { autoSwitchBitrate: { video: false } } } });
            const pin = () =>
            {
                const list = this._dash.getBitrateInfoListFor('video') || [];
                let qi = list.findIndex(b => b.height === track.height);
                if (qi < 0)
                {
                    // Lista ainda nao pronta: deriva o indice do dash.js ordenando as pistas por altura.
                    const asc = [...(this.manifest?.tracks || [])].sort((a, b) => a.height - b.height);
                    qi = asc.findIndex(t => t.height === track.height);
                }
                if (qi >= 0) this._dash.setQualityFor('video', qi, true);
                return qi >= 0 && list.length > 0;
            };
            // Aplica agora; se a lista de bitrate ainda nao estava pronta, reaplica em 300ms.
            if (!pin()) setTimeout(pin, 300);
            this.status_field.textContent = `Qualidade fixada em ${track.height}p (DASH, VP9).`;
        }
        else if (this._hlsMaster && this.hls?.levels?.length)
        {
            // Master ativo: pin manual. Mapeia pelo height (o hls.js ORDENA os niveis por
            // bitrate ASC -> 0=144p..5=1080p; a ordem do master/combo difere, entao nunca usar
            // o indice cru do combo).
            const level = this.hls.levels.findIndex(l => l.height === track.height);
            this.hls.currentLevel = level >= 0 ? level : index;
            // currentLevel recarrega do currentTime PRA FRENTE, mas o quadro EXIBIDO so
            // re-decoda quando o playback entra na regiao recarregada. Com o buffer inteiro
            // cheio (arquivos locais servidos na hora) ou com o video pausado, a troca "nao
            // aparece na tela". Um nudge minimo de currentTime forca o re-decode imediato no
            // ponto atual, ja na nova resolucao (confirmado: videoWidth muda na hora).
            const t = this.video.currentTime, dur = this.video.duration || 0;
            const nt = (dur > 0 && t + 0.05 >= dur) ? Math.max(0, t - 0.05) : t + 0.05;
            try { this.video.currentTime = nt; } catch {}
            this.status_field.textContent = `Qualidade fixada em ${track.height}p (HLS).`;
        }
        else if (track.ready && track.playlist)
        {
            // Variante ja fragmentada: limpa o buffer e passa a baixar ESTA pista agora,
            // com o player rodando (mantendo a posicao). Exatamente o que voce descreveu.
            this.play_variant(track);
        }
        else
        {
            // Ainda fragmentando esta pista: quando o track-done dela chegar, a troca
            // acontece sozinha (ver handler de 'track-done').
            this.status_field.textContent =
                `${track.height}p ainda esta sendo fragmentada — assim que ficar pronta, a troca acontece automaticamente.`;
        }

        // FLUXO DO VIDEO acompanha a resolucao selecionada no combo.
        this.update_track_stats(track.width, track.height, track.bandwidth);
        this.render_progress(track);
    }

    // Codec do arquivo enviado, no vocabulario do servidor (h264/h265/vp9/av1).
    source_codec_slug()
    {
        const c = ((this._plan && this._plan.source ? this._plan.source.vcodec : '') || 'h264').toLowerCase();
        return c === 'hevc' ? 'h265' : c;
    }

    // Converte o video enviado para um ARQUIVO unico no codec escolhido (server /convert).
    on_convert()
    {
        const folder = (this._plan && this._plan.folder) || this.session_id();
        if (!folder) return;
        // Codec da SAIDA; em branco = o do proprio arquivo, e ai o servidor so remuxa.
        const codec = this.out_codec.value || this.source_codec_slug();

        if (this.convert_source) this.convert_source.close();
        this.set_uploading(true);
        this.convert_link.hidden = true;
        this.status_field.hidden = false;
        this.status_field.textContent = `Convertendo para ${codec.toUpperCase()}… (pode demorar, VP9/AV1 sao lentos)`;

        const source = new EventSource(`/api/video/convert/${encodeURIComponent(folder)}/${encodeURIComponent(codec)}`);
        this.convert_source = source;

        source.onmessage = (message) =>
        {
            let ev;
            try { ev = JSON.parse(message.data); } catch { return; }

            if (ev.type === 'start')
            {
                this.status_field.textContent = `Convertendo para ${(ev.codec || codec).toUpperCase()} (.${ev.ext})…`;
            }
            else if (ev.type === 'done')
            {
                this.status_field.textContent = `Conversao concluida: ${ev.file}`;
                this.convert_link.hidden = false;
                this.convert_link.href = ev.file;
                this.convert_link.textContent = `Baixar ${(ev.codec || codec).toUpperCase()}`;
                this.set_uploading(false);
                source.close();
                this.convert_source = null;
            }
            else if (ev.type === 'cancelled')
            {
                this.status_field.textContent = 'Fragmentacao interrompida. A saida NAO foi publicada.';
                this.set_uploading(false);
                if (this.session_cancel_btn) this.session_cancel_btn.hidden = true;
                source.close();
                this.event_source = null;
                this.refresh_sessions_settled(this.session_id());
            }
            else if (ev.type === 'error')
            {
                this.status_field.textContent = `Falha na conversao: ${ev.message}`;
                this.set_uploading(false);
                source.close();
                this.convert_source = null;
            }
        };

        source.onerror = () =>
        {
            if (this.convert_source)
            {
                this.status_field.textContent = 'Conexao SSE perdida durante a conversao.';
                this.set_uploading(false);
                source.close();
                this.convert_source = null;
            }
        };
    }

    // Toca UMA variante ja fragmentada (playlist.m3u8 da pista) com o player rodando,
    // mantendo a posicao. Usado para trocar de resolucao ANTES do master estar pronto.
    play_variant(track)
    {
        if (!track || !track.playlist) return;
        const time = this.video.currentTime || 0;
        const was_playing = !this.video.paused;

        if (this.hls) { this.hls.destroy(); this.hls = null; }
        this._hlsMaster = false;
        const url = `${track.playlist}?v=${Date.now()}`;

        const resume = () =>
        {
            try { this.video.currentTime = time; } catch {}
            if (was_playing) this.video.play().catch(() => {});
            this.video.removeEventListener('loadedmetadata', resume);
        };
        this.video.addEventListener('loadedmetadata', resume);

        if (this.video.canPlayType('application/vnd.apple.mpegurl'))
        {
            this.video.src = url;
        }
        else if (window.Hls?.isSupported())
        {
            this.hls = new window.Hls();
            this.hls.loadSource(url);
            this.hls.attachMedia(this.video);
        }
        this.status_field.hidden = false;
        this.status_field.textContent = `Reproduzindo ${track.height}p (HLS).`;
    }

    // Numero de fragmentos da pista (real quando ja fragmentada, estimado a partir da duracao).
    segment_count(track)
    {
        if (track && track.segments) return track.segments.length;
        const dur = this.manifest?.duration || 0;
        const seg = this.manifest?.segmentDuration || 1;
        return Math.max(1, Math.ceil(dur / seg));
    }

    // Barra de progresso: marcacoes nos limites dos fragmentos + contador no fim.
    render_progress(track)
    {
        if (!this.manifest) return;
        this.progress_wrap.hidden = false;

        const count = this.segment_count(track);
        const duration = this.manifest.duration || 0;
        const seg = this.manifest.segmentDuration || 1;

        this.progress_ticks.replaceChildren();
        for (let i = 1; i < count; i++)
        {
            const tick = document.createElement('span');
            tick.className = 'progress-tick';
            tick.style.left = `${Math.min(100, (i * seg / duration) * 100)}%`;
            this.progress_ticks.append(tick);
        }

        this.fragment_counter.textContent = `${count} fragmentos`;
        this.update_progress_fill();
    }

    update_progress_fill()
    {
        const duration = this.manifest?.duration || this.video.duration || 0;
        if (!duration || !Number.isFinite(duration)) return;
        const ratio = Math.max(0, Math.min(1, (this.video.currentTime || 0) / duration));
        this.progress_fill.style.width = `${ratio * 100}%`;
    }

    // Ao passar o mouse, indica qual fragmento pertence aquela posicao.
    on_progress_hover(event)
    {
        if (!this.manifest) return;
        const rect = this.progress_bar.getBoundingClientRect();
        const ratio = Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width));
        const duration = this.manifest.duration || 0;
        const seg = this.manifest.segmentDuration || 1;
        const count = this.segment_count(this.manifest.tracks[Number(this.resolution_select.value)]);
        const time = ratio * duration;
        const index = Math.max(0, Math.min(count - 1, Math.floor(time / seg)));
        const start = index * seg;
        const end = Math.min((index + 1) * seg, duration);

        this.progress_tooltip.hidden = false;
        this.progress_tooltip.textContent =
            `Fragmento ${index + 1}/${count} · ${this.format_time(start)}–${this.format_time(end)}`;
        this.progress_tooltip.style.left = `${ratio * 100}%`;
    }

    on_progress_click(event)
    {
        const duration = this.manifest?.duration || this.video.duration || 0;
        if (!duration) return;
        const rect = this.progress_bar.getBoundingClientRect();
        const ratio = Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width));
        this.video.currentTime = ratio * duration;
    }

    format_time(seconds)
    {
        const value = Math.max(0, Math.round(seconds));
        return `${Math.floor(value / 60)}:${String(value % 60).padStart(2, '0')}`;
    }

    // ---- Barra do player (seek de reproducao) ----------------------------

    update_seek()
    {
        const duration = this.manifest?.duration || this.video.duration || 0;
        const ok = !!duration && Number.isFinite(duration);
        if (this.seek_row) this.seek_row.hidden = !ok;
        if (!ok) return;

        const current = this.video.currentTime || 0;
        const ratio = Math.max(0, Math.min(1, current / duration));
        this.seek_fill.style.width = `${ratio * 100}%`;
        this.seek_handle.style.left = `${ratio * 100}%`;
        this.seek_current.textContent = this.format_time(current);
        this.seek_total.textContent = this.format_time(duration);
    }

    seek_to_event(event)
    {
        const duration = this.manifest?.duration || this.video.duration || 0;
        if (!duration || !Number.isFinite(duration)) return;
        const rect = this.seek_bar.getBoundingClientRect();
        const ratio = Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width));
        this.video.currentTime = ratio * duration;
        this.update_seek();
    }

    // ---- Estatisticas do fluxo (codec, resolucao, audio) do protocolo ----

    update_stats(plan)
    {
        const source = plan?.source || {};
        const set = (id, value) =>
        {
            const el = this.shadowRoot.getElementById(id);
            if (el) el.textContent = (value === undefined || value === null || value === '') ? '—' : value;
        };

        set('stat-vcodec',     source.vcodec ? source.vcodec.toUpperCase() : '—');
        set('stat-resolution', (plan?.width && plan?.height) ? `${plan.width}×${plan.height}` : '—');
        set('stat-fps',        this.format_fps(source.fps));
        set('stat-vbitrate',   this.format_bitrate(source.vbitrate));
        set('stat-pixfmt',     source.pixfmt || '—');
        set('stat-acodec',     source.acodec ? source.acodec.toUpperCase() : 'sem áudio');
        set('stat-achannels',  source.achannels ? this.format_channels(source.achannels) : '—');
        set('stat-asample',    source.asamplerate ? this.format_samplerate(source.asamplerate) : '—');
        set('stat-duration',   plan?.duration ? this.format_time(plan.duration) : '—');

        // So reexibe se o usuario nao tiver fechado o painel (clique fora).
        if (this.stats_panel && !this.stats_dismissed) this.stats_panel.hidden = false;
    }

    // Atualiza apenas os campos que mudam por resolucao/fragmento (resolucao e
    // bitrate). Codec/pixel/fps/audio vem da fonte e sao estaveis. Nao mexe na
    // visibilidade — se o painel estiver aberto, os numeros trocam ao vivo.
    update_track_stats(width, height, bitrate)
    {
        const set = (id, value) =>
        {
            const el = this.shadowRoot.getElementById(id);
            if (el) el.textContent = value;
        };
        if (width && height) set('stat-resolution', `${width}×${height}`);
        const formatted = this.format_bitrate(bitrate);
        if (formatted !== '—') set('stat-vbitrate', formatted);
    }

    // Atualiza o codec exibido no painel para o que esta REALMENTE em reproducao (saida),
    // nao o da fonte. So e chamado quando o HLS/DASH renderiza de fato uma variante.
    set_codec_stats(vcodec, acodec)
    {
        const set = (id, val) => { const el = this.shadowRoot.getElementById(id); if (el && val) el.textContent = val; };
        set('stat-vcodec', vcodec);
        set('stat-acodec', acodec);
    }

    // ---- Feedback da fragmentacao por pista (checklist) -------------------
    // Uma linha por resolucao: aguardando -> fragmentando… -> pronto ✓. A selecao
    // de qualidade so e liberada quando TODAS ficam prontas (ver evento 'done').

    // ================================================================
    //  Volume
    // ================================================================
    //
    //  O <video> nao tem o atributo controls, entao nada disso vem de graca: mudo, barra,
    //  teclas e a memoria entre visitas sao daqui.
    //
    //  Guardar em localStorage vale a pena porque a alternativa e o video voltar a tocar
    //  no volume cheio toda vez que a pagina recarrega. O acesso vai em try/catch: em aba
    //  anonima, com dados do site bloqueados, o proprio LEITOR lanca excecao -- nao basta
    //  tratar o valor ausente.

    volume_aplicar(v, tambem_desmudar)
    {
        const vol = Math.min(1, Math.max(0, v));
        this.video.volume = vol;
        // Mexer na barra e um pedido de som: sair do mudo junto e o que se espera.
        if (tambem_desmudar !== false && vol > 0) this.video.muted = false;
        if (vol === 0) this.video.muted = true;
        this.volume_guardar();
        // Sincroniza AQUI, e nao so pelo volumechange: o evento e assincrono e, se o
        // elemento de video for recriado, o ouvinte fica preso no antigo. Chamar direto
        // garante que a barra e o icone acompanham sempre.
        this.volume_sincronizar();
    }

    volume_passo(d)
    {
        const base = this.video.muted ? 0 : this.video.volume;
        this.volume_aplicar(base + d);
    }

    mudo_alternar()
    {
        // Sair do mudo com o volume em zero nao devolveria som nenhum: restaura o ultimo
        // valor audivel, ou meio volume se nunca houve um.
        if (this.video.muted)
        {
            this.video.muted = false;
            if (this.video.volume === 0) this.video.volume = this._vol_anterior || 0.5;
        }
        else
        {
            if (this.video.volume > 0) this._vol_anterior = this.video.volume;
            this.video.muted = true;
        }
        this.volume_guardar();
        this.volume_sincronizar();
    }

    volume_sincronizar()
    {
        const mudo = this.video.muted || this.video.volume === 0;
        const vol  = mudo ? 0 : this.video.volume;

        if (this.volume_slider)
        {
            this.volume_slider.value = Math.round(vol * 100);
            this.volume_slider.style.setProperty('--vol-pct', Math.round(vol * 100) + '%');
        }
        if (this.mute_btn)
        {
            const nivel = mudo ? 'mudo' : (vol < 0.5 ? 'baixo' : 'alto');
            this.mute_btn.dataset.nivel = nivel;
            this.mute_btn.setAttribute('aria-pressed', mudo ? 'true' : 'false');
            this.mute_btn.setAttribute('aria-label', mudo ? 'Tirar do mudo' : 'Mudo');
            this.mute_btn.title = mudo ? 'Tirar do mudo (M)' : 'Mudo (M)';
        }
    }

    volume_guardar()
    {
        try
        {
            localStorage.setItem('vp.volume', String(this.video.volume));
            localStorage.setItem('vp.muted',  this.video.muted ? '1' : '0');
        }
        catch { /* sem armazenamento: o volume vale so para esta visita */ }
    }

    volume_restaurar()
    {
        let vol = 1, mudo = false;
        try
        {
            const v = localStorage.getItem('vp.volume');
            const m = localStorage.getItem('vp.muted');
            if (v !== null && isFinite(parseFloat(v))) vol = Math.min(1, Math.max(0, parseFloat(v)));
            if (m !== null) mudo = (m === '1');
        }
        catch { /* idem */ }

        this.video.volume = vol;
        this.video.muted  = mudo;
        this._vol_anterior = vol > 0 ? vol : 0.5;
        this.volume_sincronizar();
    }

    // ================================================================
    //  Tela cheia
    // ================================================================
    //
    //  Vai para tela cheia a CASCA (#player-shell = video + barra de controles). Mandar o
    //  proprio <video> entregaria a tela ao navegador, que desenha os controles nativos
    //  dele -- e a barra de busca, o seletor de qualidade e os botoes daqui sumiriam.

    digitando(e)
    {
        const alvo = e.composedPath ? e.composedPath()[0] : e.target;
        if (!alvo || !alvo.tagName) return false;
        const t = alvo.tagName.toUpperCase();
        return t === 'INPUT' || t === 'TEXTAREA' || t === 'SELECT' || alvo.isContentEditable === true;
    }

    tela_cheia_ativa()
    {
        const el = document.fullscreenElement || document.webkitFullscreenElement || null;
        // Em shadow DOM o fullscreenElement do documento e o HOST, nao o elemento interno.
        return el === this || el === this.player_shell;
    }

    async tela_cheia_alternar()
    {
        if (!this.player_shell) return;
        try
        {
            if (this.tela_cheia_ativa())
            {
                const sair = document.exitFullscreen || document.webkitExitFullscreen;
                if (sair) await sair.call(document);
            }
            else
            {
                const pedir = this.player_shell.requestFullscreen
                           || this.player_shell.webkitRequestFullscreen;
                if (pedir) await pedir.call(this.player_shell);
            }
        }
        catch (err)
        {
            // Sem permissao (gesto do usuario exigido) ou nao suportado: avisa em vez de
            // falhar calado, senao o botao parece quebrado.
            if (this.status_field) this.status_field.textContent = `Tela cheia indisponivel: ${err.message}`;
        }
        this.tela_cheia_sincronizar();
    }

    tela_cheia_sincronizar()
    {
        const ativa = this.tela_cheia_ativa();
        if (this.fullscreen_btn)
        {
            this.fullscreen_btn.setAttribute('aria-pressed', ativa ? 'true' : 'false');
            this.fullscreen_btn.setAttribute('aria-label', ativa ? 'Sair da tela cheia' : 'Tela cheia');
            this.fullscreen_btn.title = ativa
                ? 'Sair da tela cheia (F ou Esc)'
                : 'Tela cheia (F, ou duplo clique no video). Esc sai.';
        }
    }

    // ================================================================
    //  Cabecalho das faixas: progresso geral, decorrido e estimativa
    // ================================================================
    //
    //  O DECORRIDO e cronometrado aqui, no cliente, e nao vem do servidor: o servidor so
    //  manda 'elapsed' no fim. Para o numero andar de segundo em segundo enquanto o job
    //  roda, o relogio tem de ser local.
    //
    //  O TOTAL ESTIMADO e uma projecao do ritmo ate agora (decorrido / fracao concluida).
    //  E so isso mesmo: no comeco ela oscila muito, entao a estimativa so aparece depois
    //  de 5% e de 3 segundos. Antes disso mostra "estimando...", que e honesto, em vez de
    //  um numero que muda de minuto para segundo na cara de quem esta olhando.
    //
    //  No fim o cabecalho troca a estimativa pelo tempo MEDIDO que o servidor informou.

    frag_head_mmss(seg)
    {
        if (!isFinite(seg) || seg < 0) return '--:--';
        // Arredonda o TOTAL antes de dividir, e nao cada parte: assim 185.6 s vira
        // 3:06 igual ao "3m 06s" que a linha de conclusao ja mostrava. Truncando os
        // segundos, os dois textos discordavam em 1 s para o mesmo valor.
        const t = Math.round(seg);
        const m = Math.floor(t / 60);
        const s = t % 60;
        return `${m}:${String(s).padStart(2, '0')}`;
    }

    frag_head_iniciar()
    {
        if (!this.frag_head) return;
        this.frag_head_parar();

        this._frag_t0       = performance.now();
        this._frag_pct      = 0;
        this._frag_prontas  = 0;

        this.frag_head.hidden = false;
        this.frag_head.classList.remove('concluido');
        this.frag_head_pct.textContent       = '0%';
        this.frag_head_fill.style.width      = '0%';
        this.frag_head_decorrido.textContent = '0:00';
        this.frag_head_total.textContent     = 'estimando...';

        this._frag_timer = setInterval(() => this.frag_head_tick(), 500);
    }

    frag_head_parar()
    {
        if (this._frag_timer) { clearInterval(this._frag_timer); this._frag_timer = null; }
    }

    frag_head_decorrido_seg()
    {
        return this._frag_t0 ? (performance.now() - this._frag_t0) / 1000 : 0;
    }

    frag_head_tick()
    {
        if (!this.frag_head || this.frag_head.hidden) return;
        const dec = this.frag_head_decorrido_seg();
        this.frag_head_decorrido.textContent = this.frag_head_mmss(dec);

        if (this._frag_pct >= 5 && dec >= 3)
        {
            const total = dec / (this._frag_pct / 100);
            this.frag_head_total.textContent = '~' + this.frag_head_mmss(total);
        }
    }

    // pct: 0..100 geral. Chamado pelos eventos de progresso e de pista concluida.
    frag_head_progresso(pct)
    {
        if (!this.frag_head || this.frag_head.hidden) return;
        // Nunca anda para tras: com progresso por pista o percentual da pista reinicia a
        // cada nova, e o geral voltaria visualmente sem que nada tenha se perdido.
        this._frag_pct = Math.max(this._frag_pct || 0, Math.min(100, Math.max(0, pct)));
        this.frag_head_pct.textContent  = Math.round(this._frag_pct) + '%';
        this.frag_head_fill.style.width = this._frag_pct + '%';
        this.frag_head_tick();
    }

    // Progresso geral a partir do progresso de UMA pista, quando o servidor manda por pista.
    frag_head_progresso_pista(pctPista)
    {
        const total = this._readyTotal || 1;
        const feito = (this._frag_prontas || 0) + Math.min(1, Math.max(0, pctPista / 100));
        this.frag_head_progresso((feito / total) * 100);
    }

    frag_head_pista_pronta()
    {
        this._frag_prontas = (this._frag_prontas || 0) + 1;
        const total = this._readyTotal || 1;
        this.frag_head_progresso((this._frag_prontas / total) * 100);
    }

    // segServidor: o 'elapsed' medido pelo servidor. Sem ele, cai no relogio local.
    frag_head_concluir(segServidor)
    {
        if (!this.frag_head) return;
        this.frag_head_parar();

        const seg = (typeof segServidor === 'number' && segServidor > 0)
                  ? segServidor : this.frag_head_decorrido_seg();

        this.frag_head.classList.add('concluido');
        this.frag_head.hidden = false;
        this.frag_head_pct.textContent       = '100%';
        this.frag_head_fill.style.width      = '100%';
        this.frag_head_decorrido.textContent = this.frag_head_mmss(seg);
        this.frag_head_total.textContent     = 'total';
        this._frag_pct = 100;
    }

    frag_head_interromper(rotulo)
    {
        if (!this.frag_head) return;
        this.frag_head_parar();
        this.frag_head_total.textContent = rotulo || 'interrompido';
    }

    reset_track_progress(total)
    {
        if (this.track_progress) this.track_progress.replaceChildren();
        this._track_rows = {};
        this._readyTotal = total || 0;
        this._readyCount = 0;
    }

    // Cria a linha da pista sob demanda (o checklist se monta pelos eventos SSE:
    // 1 pista no HLS passthrough, N no DASH). Assim casa com o que o server emite.
    ensure_track_row(name, label)
    {
        if (!this.track_progress || (this._track_rows && this._track_rows[name])) return;
        if (!this._track_rows) this._track_rows = {};
        const row = document.createElement('div');
        row.className = 'virtual-track is-pending';
        row.innerHTML =
            `<span class="track-name">${label}</span>` +
            `<span class="track-limit"><i></i></span>` +
            `<span class="track-meta">aguardando</span>` +
            `<span class="track-encoder" hidden></span>`;
        this.track_progress.append(row);
        this._track_rows[name] = row;
    }

    set_track_status(name, status)
    {
        const row = this._track_rows && this._track_rows[name];
        if (!row) return;
        row.classList.remove('is-pending', 'is-active', 'is-done');
        const meta = row.querySelector('.track-meta');
        if (status === 'active')     { row.classList.add('is-active'); if (meta) meta.textContent = 'fragmentando…'; }
        else if (status === 'done')  { row.classList.add('is-done');   if (meta) meta.textContent = 'pronto ✓'; }
        else                         { row.classList.add('is-pending');if (meta) meta.textContent = 'aguardando'; }
        this._readyCount = Object.values(this._track_rows).filter(r => r.classList.contains('is-done')).length;
    }

    // Mostra QUEM codificou a pista: "mf-hw [hardware] NVIDIA H.264 Encoder MFT",
    // "x265 [threads] ...". O degrau entre colchetes vira data-tier, para destacar quando
    // a GPU foi usada -- e deixar visivel quando nao foi.
    set_track_encoder(name, text)
    {
        const row = this._track_rows && this._track_rows[name];
        const el = row && row.querySelector('.track-encoder');
        if (!el) return;
        const a = text.indexOf('['), b = text.indexOf(']', a + 1);
        el.dataset.tier = (a >= 0 && b > a) ? text.slice(a + 1, b) : '';
        el.textContent = `encoder: ${text}`;
        el.hidden = false;
    }

    format_fps(raw)
    {
        if (!raw) return '—';
        const parts = String(raw).split('/');
        if (parts.length === 2)
        {
            const num = parseFloat(parts[0]), den = parseFloat(parts[1]);
            if (den > 0) return `${(num / den).toFixed(2).replace(/\.?0+$/, '')} fps`;
        }
        const value = parseFloat(raw);
        return Number.isFinite(value) ? `${value} fps` : '—';
    }

    format_bitrate(bps)
    {
        const value = Number(bps) || 0;
        if (value <= 0) return '—';
        return value >= 1e6 ? `${(value / 1e6).toFixed(1)} Mb/s` : `${Math.round(value / 1000)} kb/s`;
    }

    format_channels(count)
    {
        const n = Number(count) || 0;
        if (n === 1) return 'Mono';
        if (n === 2) return 'Estéreo';
        return `${n} canais`;
    }

    format_samplerate(hz)
    {
        const value = Number(hz) || 0;
        if (value <= 0) return '—';
        const khz = value / 1000;
        return `${Number.isInteger(khz) ? khz : khz.toFixed(1)} kHz`;
    }

    disconnectedCallback()
    {
        if (this._on_fs_change)
        {
            document.removeEventListener('fullscreenchange', this._on_fs_change);
            document.removeEventListener('webkitfullscreenchange', this._on_fs_change);
        }
        if (this._on_doc_click)
        {
            document.removeEventListener('click', this._on_doc_click);
            this._on_doc_click = null;
        }
        if (this._dash) { try { this._dash.destroy(); } catch {} this._dash = null; }
        if (this.object_url)
        {
            URL.revokeObjectURL(this.object_url);
            this.object_url = null;
        }
    }


    setVideoSource(url)
    {
        if (this.sourceElement && this.videoElement)
        {
            this.sourceElement.src = url;
            this.videoElement.load();
            this.videoElement.play();
        }
    }


    on_video_ended()
    {
        console.log('Video has ended.');

    }


    update_player_control(option)
    {
        if (option == PlayerControlOptions.STOP || option == PlayerControlOptions.NONE)
        {
            this.play_img.src = new URL('./resources/images/play.svg', import.meta.url);
            this.play_img.alt = 'Play';
        }
        else if (option == PlayerControlOptions.PLAY)
        {
            this.play_img.src = new URL('./resources/images/pause.svg', import.meta.url);
            this.play_img.alt = 'Pause';
        }
    }

}


customElements.define('video-player', VideoPlayer);