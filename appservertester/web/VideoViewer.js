import { WebRouter } from './webapprouter/WebRouter.js';
import { AuthorizationStore } from './storage/AuthorizationStore.js';
import { GetWebApiBaseAddress, SetWebApiBaseAddress } from './storage/GlobalData.js';
import { showWait, hideWait } from './waitdialog/wait-box.js';)


//SetWebApiBaseAddress("http://" + window.location.hostname);
SetWebApiBaseAddress("http://localhost:1234");


const Authorization = new AuthorizationStore();


const video         = document.getElementById('player');
const playBtn       = document.getElementById('play');
const playIcon      = document.getElementById('playIcon');
const iconPlay      = document.getElementById('icon-play');
const iconPause     = document.getElementById('icon-pause');
const label         = document.getElementById('label');

let loaded = false; // define se já carregamos a fonte fixa

function OnLoadWindow()
{
    auto_authorize();
}
window.onload = OnLoadWindow;



// URL de vídeo público (fixo) — altere se quiser testar outro


function updateUIPlaying(isPlaying)
{
    if (isPlaying)
    {
        playIcon.src      = 'resources/images/pause.svg';
        playIcon.alt      = 'Pause';
        label.textContent = 'Pause';
        playBtn.setAttribute('aria-pressed', 'true');
    }
    else
    {
        playIcon.src      = 'resources/images/play.svg';
        playIcon.alt      = 'Play';
        label.textContent = 'Play';
        playBtn.setAttribute('aria-pressed', 'false');
    }
}


playBtn.addEventListener('click', () =>
{
    if (!loaded)
    {
        // primeiro clique: define a fonte fixa
        video.src = VIDEO_URL;
        loaded = true;
    }
    if (video.paused)
    {
        video.play().then(() => updateUIPlaying(true)).catch(err =>
        {
            console.warn('Falha ao reproduzir:', err);
        });
    }
    else
    {
        video.pause();
        updateUIPlaying(false);
    }
});


// Sincroniza ícone ao pausar/continuar por outros meios (ex.: atalho do teclado)
video.addEventListener('play',  () => updateUIPlaying(true));
video.addEventListener('pause', () => updateUIPlaying(false));
video.addEventListener('ended', () => updateUIPlaying(false));


// Atalho: Espaço para Play/Pause após carregar
window.addEventListener('keydown', (e) =>
{
    if (e.code === 'Space') {
        e.preventDefault();
        if (!loaded) {
            video.src = VIDEO_URL;
            loaded = true;
        }
        if (video.paused) { video.play(); } else { video.pause(); }
    }
});
