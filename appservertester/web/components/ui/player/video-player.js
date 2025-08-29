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

    constructor()
    {
        super();
        this.attachShadow({ mode: 'open' });
    }


    async connectedCallback()
    {
        const html_url = new URL('./video-player.html', import.meta.url);
        const css_url  = new URL('./video-player.css', import.meta.url);
        const html     = await fetch(html_url).then(r => r.text());
        const css      = await fetch(css_url).then(r => r.text());

        this.shadowRoot.innerHTML = `<style>${css}</style>${html}`;

        this.video                 = this.shadowRoot.querySelector('#player');
        this.source_field          = this.shadowRoot.querySelector('#player-source');
        this.source_button         = this.shadowRoot.querySelector('#player-open-file');
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
        this.stop_img.src          = new URL('./resources/images/play.svg', import.meta.url);
        this.back_img.src          = new URL('./resources/images/back.svg', import.meta.url);
        this.back_step_img.src     = new URL('./resources/images/back.svg', import.meta.url);
        this.play_img.src          = new URL('./resources/images/play.svg', import.meta.url);
        this.forward_step_img.src  = new URL('./resources/images/forward.svg', import.meta.url);
        this.forward_img.src       = new URL('./resources/images/forward.svg', import.meta.url);


        this.video.addEventListener('ended', () => this.on_video_ended());
        this.play.addEventListener('click', () => this.on_play());
        this.stop.addEventListener('click', () => this.on_stop());
        this.back.addEventListener('click', () => this.on_back());
        this.forward.addEventListener('click', () => this.on_forward());
        this.back_step.addEventListener('click', () => this.on_back_step());
        this.forward_step.addEventListener('click', () => this.on_forward_step());
        this.source_button.addEventListener('click', () => this.on_open_file());

        window.addEventListener('keydown', (e) =>
        {
            if (e.code === 'Space')
            {
                e.preventDefault();
                this.on_play();
            }
        });

        this.video.addEventListener('play',  () => this.update_player_control(PlayerControlOptions.PLAY));
        this.video.addEventListener('pause', () => this.update_player_control(PlayerControlOptions.PAUSE));
        this.video.addEventListener('ended', () => this.update_player_control(PlayerControlOptions.STOP));
        //...
    }


    on_play()
    {
        if (video.paused)
        {
            video.play().then(() => update_player_control(PlayerControlOptions.PLAY)).catch(err =>
            {
                console.warn('Falha ao reproduzir:', err);
            });
        }
        else
        {
            video.pause();
            update_player_control(PlayerControlOptions.PAUSE);
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
            playIcon.src = 'resources/images/play.svg';
            playIcon.alt = 'Play';
            label.textContent = 'Play';
            playBtn.setAttribute('aria-pressed', 'false');
        }
        else if (option == PlayerControlOptions.PLAY)
        {
            playIcon.src = 'resources/images/pause.svg';
            playIcon.alt = 'Pause';
            label.textContent = 'Pause';
            playBtn.setAttribute('aria-pressed', 'true');
        }
    }

}


customElements.define('video-player', VideoPlayer);