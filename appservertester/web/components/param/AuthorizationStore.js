export class AuthorizationStore
{
    static AUTHORIZATION   = 'authorization';

    UserName        = '';
    Password        = '';
    SessionUID      = '';
    IsKeepConnected = false;

    constructor()
    {
        this.load()
    }

    save()
    {
        sessionStorage.setItem(AuthorizationStore.AUTHORIZATION, null);
        localStorage.setItem(AuthorizationStore.AUTHORIZATION, null);

        if (this.IsKeepConnected == true) {
            localStorage.setItem(AuthorizationStore.AUTHORIZATION, JSON.stringify(this));
        }
        else {
            sessionStorage.setItem(AuthorizationStore.AUTHORIZATION, JSON.stringify(this));
        }
    }

    load()
    {
        const se = sessionStorage.getItem(AuthorizationStore.AUTHORIZATION);

        if (se != null && se != 'null')
        {
            this.#populate(JSON.parse(se));
        }
        else {
            const sa = localStorage.getItem(AuthorizationStore.AUTHORIZATION);

            if (sa != null && sa != 'null')
            {
                this.#populate(JSON.parse(sa));
            }
            else {
                this.reset();
            }
        }
    }


    #populate(obj)
    {
        if (obj && typeof obj === 'object')
        {
            this.IsKeepConnected = obj.IsKeepConnected;
            this.UserName        = obj.UserName;
            this.Password        = obj.Password;
            this.SessionUID      = obj.SessionUID;
        }
        else
        {
            this.reset();
        }
    }

    reset()
    {
        this.IsKeepConnected = false;
        this.UserName        = '';
        this.Password        = '';
        this.SessionUID      = '';
    }
}