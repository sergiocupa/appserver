
export class WaitingBox extends HTMLElement
{
    constructor()
    {
        super();

        this.attachShadow({ mode: 'open' });
    }

    async connectedCallback()
    {
        const html = await fetch('waiting-box.html').then(r => r.text());
        const css  = await fetch('waiting-box.css').then(r => r.text());

        this.shadowRoot.innerHTML = '<style>${css}</style>${html}';
    }


    async show(text)
    {
        document.getElementById('modal-wait').style.display      = 'block';
        document.getElementById('waiting-box-content').innerText = text;
    }

    async hide()
    {
        document.getElementById('modal-wait').style.display = 'none';
    }
}

customElements.define('waiting-box', WaitingBox);