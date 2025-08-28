
export const PlayerControlOptions = Object.freeze({ NONE: '', STOP: 'STOP', PLAY: 'PLAY', PAUSE: 'PAUSE', FORWARD: "FORWARD", BACK: "BACK", FORWARD_STEP = "FORWARD-STEP", BACK_STEP: "BACK-STEP"  });

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
        const html = await fetch('video-player.html').then(r => r.text());
        const css  = await fetch('video-player.css').then(r => r.text());

        this.shadowRoot.innerHTML = `<style>${css}</style>${html}`;

        this.video         = this.shadowRoot.querySelector('player');
        this.stop          = this.shadowRoot.querySelector('stop');
        this.play          = this.shadowRoot.querySelector('play');
        this.back          = this.shadowRoot.querySelector('back');
        this.forward       = this.shadowRoot.querySelector('forward');
        this.back_step     = this.shadowRoot.querySelector('back-step');
        this.forward_step  = this.shadowRoot.querySelector('forward-step');
        this.source_field  = this.shadowRoot.querySelector('player-source');
        this.source_button = this.shadowRoot.querySelector('player-open-file');
        
        this.video.addEventListener('ended', () => this.onVideoEnded());

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

        video.addEventListener('play',  () => update_player_control(PlayerControlOptions.PLAY));
        video.addEventListener('pause', () => update_player_control(PlayerControlOptions.PAUSE));
        video.addEventListener('ended', () => update_player_control(PlayerControlOptions.STOP));
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