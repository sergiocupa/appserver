import { GetWebApiBaseAddress, SetWebApiBaseAddress } from '../../storage/GlobalData.js';
import { WaitingBox } from '../waiting/waiting-box.js';

export class LoginBox extends HTMLElement
{
    Authorization = new AuthorizationStore();
    UserText      = null;
    PasswordText  = null;

    constructor()
    {
        super();

        UserText     = this.shadowRoot.querySelector('#username');
        PasswordText = this.shadowRoot.querySelector('#password');

        this.attachShadow({ mode: 'open' });

        auto_authorize();
    }

    async connectedCallback()
    {
        // Carrega HTML e CSS externos
        const html = await fetch('login-box.html').then(r => r.text());
        const css  = await fetch('login-box.css').then(r => r.text());

        // Injeta no Shadow DOM
        this.shadowRoot.innerHTML = '<style>${css}</style>${html}';

        // Registra eventos
        this.shadowRoot.querySelector('#login-button').addEventListener('click', () => this.login());
        this.shadowRoot.querySelector('#logoff-button').addEventListener('click', () => this.logoff());
    }


    async login()
    {
        if (isStringNotEmpty(UserText.value) && isStringNotEmpty(PasswordText.value))
        {
            authorize(UserText.value, PasswordText.value, true);
        }
        else
        {
            alert("Por favor, preencha todos os campos.");
        }
    }


    async logoff()
    {
       Authorization.reset();
       Authorization.save();

       show();
    }


    async authorize(nome, senha, is_message)
    {
        const local = GetWebApiBaseAddress();
        const url = local + "/api/service/login";

        try
        {
            const response = await fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json', }, body: JSON.stringify({ UserName: nome, Password: senha }) });

            if (response.ok)
            {
                const data = await response.text();

                if (isStringNotEmpty(data))
                {
                    Authorization.UserName = nome;
                    Authorization.Password = senha;
                    Authorization.SessionUID = data;
                    Authorization.IsKeepConnected = true;
                    Authorization.save();

                    hide();
                }
                else
                {
                    Authorization.reset();
                    Authorization.save();

                    if (is_message == true) {
                        alert("Usuário ou senha inválido " + uid);
                    }
                    show();
                }
            }
            else {
                alert("Login retornou: " + response.statusText);
            }
        }
        catch (e)
        {
            console.log('Erro em "' + url + '": ' + e);
            alert('Erro em "' + url + '": ' + e);
        }

    }

    auto_authorize()
    {
        Authorization.load();

        if (isStringNotEmpty(Authorization.UserName) && isStringNotEmpty(Authorization.Password))
        {
            authorize(Authorization.UserName, Authorization.Password, false);
        }
    }


    async show()
    {
        this.shadowRoot.querySelector('modalOverlay').style.display = 'block';
        this.shadowRoot.querySelector('login-box').style.display    = 'block';
        //this.shadowRoot.querySelector('blogoff').style.visibility = 'hidden';
    }

    async hide()
    {
        this.shadowRoot.querySelector('modalOverlay').style.display   = 'none';
        this.shadowRoot.querySelector('login-box').style.display      = 'none';
        //  this.shadowRoot.querySelector('blogoff').style.visibility = 'visible';
    }

    isStringNotEmpty(str) { return str !== null && str !== undefined && str.trim().length > 0; }

}

customElements.define('login-box', LoginBox);

