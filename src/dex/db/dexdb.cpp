#include <ctime>
#include <utility>
#include "dexdb.h"
#include "dexdbexception.h"
#include "defaultdatafordb.h"
#include <boost/filesystem.hpp>

#include "base58.h"
#include "random.h"
#include "util.h"

namespace dex {

std::map<CallBackDB*, int> DexDB::callBack;
DexDB *DexDB::pSingleton = nullptr;
int DexDB::nCounter = 0;
bool DexDB::bOffersRescan = false;


DexDB::DexDB()
{
    db = sqlite3pp::database(strDexDbFile.c_str(),
            SQLITE_OPEN_READWRITE |
            SQLITE_OPEN_CREATE |
            SQLITE_OPEN_FULLMUTEX |
            SQLITE_OPEN_SHAREDCACHE);

    db.set_busy_timeout(DEFAULT_DEX_BUSY_TIMEOUT);

    isGetCountriesDataFromDB = true;
    isGetCurrenciesDataFromDB = true;
    isGetPaymentsDataFromDB = true;

    if (isDexDbEmpty()) {
        createTables(db);
        createIndexes(db);
        addDefaultData();
    }

    checkDexDbIntegrity();

    if (isDexDbOutdated()) {
        sqlite3pp::transaction tx(db);
        dropIndexes();
        renameTables();
        dropTables();
        createTables(db);
        moveTablesData();
        createIndexes(db);
        dropOldTables();
        addDefaultData();
        tx.commit();
    } else {
        checkDexDbSchema();
    }
}

DexDB::~DexDB()
{
}


DexDB *DexDB::instance()
{
    if (pSingleton == nullptr) {
        pSingleton = new DexDB();
    }

    nCounter++;

    return pSingleton;
}

void DexDB::freeInstance()
{
    if (nCounter > 0) {
        nCounter--;

        if (nCounter == 0) {
            delete pSingleton;
            pSingleton = nullptr;
        }
    }
}

DexDB *DexDB::self()
{
    return pSingleton;
}

std::string DexDB::getErrMsg()
{
    std::string result;
    char const* err = db.error_msg();
    if (err != 0) {
        result = err;
    }
    return result;
}


void DexDB::addCallBack(CallBackDB *callBack)
{
     this->callBack[callBack]++;
}

void DexDB::removeCallBack(CallBackDB *callBack)
{
    std::map<CallBackDB*,int>::iterator it = this->callBack.find(callBack);

    if (it != this->callBack.end()) {
        if (it->second > 0) {
            it->second--;
            if (it->second == 0) {
                this->callBack.erase(it);
            }
        }
    }
}

sqlite3pp::database * DexDB::getDB()
{
  return &db;
}

bool DexDB::isDexDbOutdated()
{
    sqlite3pp::query qry(db, "SELECT version FROM dbversion");
    sqlite3pp::query::iterator i = qry.begin();

    int iDBversion = 0; 
    std::tie(iDBversion) = (*i).get_columns<int>(0);

    if (dex::uiDexDBversionInCode != (unsigned int)iDBversion)
        return true;  // dex DB version is old!
    else
       return false;
}


bool DexDB::isDexDbEmpty()
{
    int count = 0;
    sqlite3pp::query qry(db, "SELECT count(*) FROM sqlite_master");
    sqlite3pp::query::iterator it = qry.begin();
    (*it).getter() >> count;
    return count == 0;
}


void DexDB::checkDexDbSchema()
{
    sqlite3pp::database dbm = sqlite3pp::database(":memory:");
    createTables(dbm);
    createIndexes(dbm);

    std::map<std::string,std::string> schema1 = getDbSchema(db);
    std::map<std::string,std::string> schema2 = getDbSchema(dbm);

    if(schema1.size() != schema2.size()) {
        throw sqlite3pp::database_error("DEX db schema is incorrect");
    }

    for(auto i = schema1.begin(), j = schema2.begin(); i != schema1.end(); ++i, ++j) {
        if(*i != *j) throw sqlite3pp::database_error("DEX db schema is incorrect");
    }
}


std::map<std::string,std::string> DexDB::getDbSchema(sqlite3pp::database &db)
{
    std::map<std::string,std::string> result;
    sqlite3pp::query qry(db, "SELECT name, sql FROM sqlite_master WHERE sql NOT NULL");

    for (sqlite3pp::query::iterator it = qry.begin(); it != qry.end(); ++it) {
        std::string name, sql;
        std::tie(name, sql) = (*it).get_columns<std::string, std::string>(0, 1);
        result[name] = sql;
    }
    return result;
}


void DexDB::checkDexDbIntegrity()
{
    std::string result;
    sqlite3pp::query qry(db, "PRAGMA integrity_check");

    for (auto it : qry) {
        std::string str;
        it.getter() >> str;
        result += str;
    }
    if (result != "ok") {
        throw sqlite3pp::database_error(result.c_str());
    }
}


void DexDB::dropTables()
{
    db.execute("DROP TABLE IF EXISTS dbversion");
    db.execute("DROP TABLE IF EXISTS countries");
    db.execute("DROP TABLE IF EXISTS currencies");
    db.execute("DROP TABLE IF EXISTS paymentMethods");
    db.execute("DROP TABLE IF EXISTS myOffers");
    db.execute("DROP TABLE IF EXISTS offersSell");
    db.execute("DROP TABLE IF EXISTS offersBuy");
}

void DexDB::dropOldTables()
{
    db.execute("DROP TABLE IF EXISTS countries_old");
    db.execute("DROP TABLE IF EXISTS currencies_old");
    db.execute("DROP TABLE IF EXISTS paymentMethods_old");
    db.execute("DROP TABLE IF EXISTS myOffers_old");
    db.execute("DROP TABLE IF EXISTS offersSell_old");
    db.execute("DROP TABLE IF EXISTS offersBuy_old");
}

void DexDB::moveTablesData()
{
    db.execute("INSERT INTO countries      SELECT * FROM countries_old");
    db.execute("INSERT INTO currencies     SELECT * FROM currencies_old");
    db.execute("INSERT INTO paymentMethods SELECT * FROM paymentMethods_old");

    db.execute("INSERT INTO myOffers SELECT hash, idTransaction, pubKey, countryIso, currencyIso, "
               "paymentMethod, price, minAmount, timeCreate, timeToExpiration, timeCreate, shortInfo, "
               "details, type, status, editingVersion, editsign FROM myOffers_old");

    db.execute("INSERT INTO offersSell SELECT idTransaction, hash, pubKey, countryIso, currencyIso, "
               "paymentMethod, price, minAmount, timeCreate, timeToExpiration, timeCreate, shortInfo, "
               "details, editingVersion, editsign FROM offersSell_old");

    db.execute("INSERT INTO offersBuy SELECT idTransaction, hash, pubKey, countryIso, currencyIso, "
               "paymentMethod, price, minAmount, timeCreate, timeToExpiration, timeCreate, shortInfo, "
               "details, editingVersion, editsign FROM offersBuy_old");
}

void DexDB::dropIndexes()
{
    db.execute("DROP INDEX IF EXISTS idx_offersSell_timeexp");
    db.execute("DROP INDEX IF EXISTS idx_offersBuy_timeexp");
    db.execute("DROP INDEX IF EXISTS idx_offersMy_timeexp");
    db.execute("DROP INDEX IF EXISTS hash_editing_version_buy");
    db.execute("DROP INDEX IF EXISTS hash_editing_version_sell");
    db.execute("DROP INDEX IF EXISTS idx_offersSell_timemod");
    db.execute("DROP INDEX IF EXISTS idx_offersBuy_timemod");
}


void DexDB::renameTables()
{
    db.execute("ALTER TABLE countries RENAME TO countries_old");
    db.execute("ALTER TABLE currencies RENAME TO currencies_old");
    db.execute("ALTER TABLE paymentMethods RENAME TO paymentMethods_old");
    db.execute("ALTER TABLE myOffers RENAME TO myOffers_old");
    db.execute("ALTER TABLE offersSell RENAME TO offersSell_old");
    db.execute("ALTER TABLE offersBuy RENAME TO offersBuy_old");
}


void DexDB::addCountry(const std::string &iso, const std::string &name, const std::string &currency, const bool &enabled, const int &sortOrder)
{
    countries.push_back({iso, name, enabled});

    sqlite3pp::command cmd(db, "INSERT INTO countries (iso, name, currencyId, enabled, sortOrder) SELECT :iso, :name, "
                               "currencies.id, :enabled, :sortOrder FROM currencies WHERE iso = :currencyIso");
    cmd.bind(":iso", iso, sqlite3pp::nocopy);
    cmd.bind(":name", name, sqlite3pp::nocopy);
    cmd.bind(":enabled", enabled);
    cmd.bind(":sortOrder", sortOrder);
    cmd.bind(":currencyIso", currency, sqlite3pp::nocopy);

    int status = cmd.execute();
    finishTableOperation(Countries, Add, status);
}

void DexDB::editCountries(const std::list<CountryInfo> &list)
{
    if (countries.size() == list.size()) {
        countries = list;
    }

    int sort = 0;
    for (auto item : list) {
        int status = editCountry(item.iso, item.enabled, sort);
        if (status != 0) {
            finishTableOperation(Countries, Edit, status);
            return;
        }
        sort++;
    }

    finishTableOperation(Countries, Edit, 0);
}

void DexDB::deleteCountry(const std::string &iso)
{
    countries.remove_if([iso](CountryInfo c){return c.iso == iso;});

    sqlite3pp::command cmd(db, "DELETE FROM countries WHERE iso = ?");
    cmd.bind(1, iso, sqlite3pp::nocopy);

    int status = cmd.execute();
    finishTableOperation(Countries, Delete, status);
}

std::list<CountryInfo> DexDB::getCountriesInfo()
{
    if (isGetCountriesDataFromDB) {
        std::string query = "SELECT iso, name, enabled FROM countries ORDER BY sortOrder";

        sqlite3pp::query qry(db, query.c_str());

        for (sqlite3pp::query::iterator i = qry.begin(); i != qry.end(); ++i) {
            std::string iso;
            std::string name;
            bool enabled;
            std::tie(iso, name, enabled) = (*i).get_columns<std::string, std::string, bool>(0, 1, 2);

            CountryInfo info;
            info.iso = iso;
            info.name = name;
            info.enabled = enabled;

            countries.push_back(info);
        }

        isGetCountriesDataFromDB = false;

        int status = qry.finish();
        finishTableOperation(Countries, Read, status);
    }

    return countries;
}

CountryInfo DexDB::getCountryInfo(const std::string &iso)
{
    CountryInfo info;

    if (isGetCountriesDataFromDB) {
        std::string query = "SELECT name, enabled FROM countries WHERE iso = '" + iso + "'";
        sqlite3pp::query qry(db, query.c_str());

        sqlite3pp::query::iterator i = qry.begin();
        std::string name;
        bool enabled;
        std::tie(name, enabled) = (*i).get_columns<std::string, bool>(0, 1);

        info.iso = iso;
        info.name = name;
        info.enabled = enabled;

        int status = qry.finish();
        finishTableOperation(Countries, Read, status);
    } else {
        auto found = std::find_if(countries.begin(), countries.end(), [iso](CountryInfo info){ return info.iso == iso; });
        if (found != countries.end()) {
            info = *found;
        }
    }

    return info;
}

void DexDB::addCurrency(const std::string &iso, const std::string &name, const std::string &symbol, const bool &enabled, const int &sortOrder)
{
    currencies.push_back({iso, name, symbol, enabled});

    sqlite3pp::command cmd(db, "INSERT INTO currencies (iso, name, symbol, enabled, sortOrder) VALUES (?, ?, ?, ?, ?)");
    cmd.bind(1, iso, sqlite3pp::nocopy);
    cmd.bind(2, name, sqlite3pp::nocopy);
    cmd.bind(3, symbol, sqlite3pp::nocopy);
    cmd.bind(4, enabled);
    cmd.bind(5, sortOrder);

    int status = cmd.execute();
    finishTableOperation(Countries, Add, status);
}

void DexDB::editCurrencies(const std::list<CurrencyInfo> &list)
{
    if (currencies.size() == list.size()) {
        currencies = list;
    }

    int sort = 0;
    for (auto item : list) {
        int status = editCurrency(item.iso, item.enabled, sort);
        if (status != 0) {
            finishTableOperation(Currencies, Edit, status);
            return;
        }
        sort++;
    }

    finishTableOperation(Currencies, Edit, 0);
}

void DexDB::deleteCurrency(const std::string &iso)
{
    currencies.remove_if([iso](CurrencyInfo c){return c.iso == iso;});

    sqlite3pp::command cmd(db, "DELETE FROM currencies WHERE iso = ?");
    cmd.bind(1, iso, sqlite3pp::nocopy);

    int status = cmd.execute();
    finishTableOperation(Currencies, Delete, status);
}

std::list<CurrencyInfo> DexDB::getCurrenciesInfo()
{
    if (isGetCurrenciesDataFromDB) {
        std::string query = "SELECT iso, name, symbol, enabled FROM currencies ORDER BY sortOrder";

        sqlite3pp::query qry(db, query.c_str());

        for (sqlite3pp::query::iterator i = qry.begin(); i != qry.end(); ++i) {
            std::string iso;
            std::string name;
            std::string symbol;
            bool enabled;
            std::tie(iso, name, symbol, enabled) = (*i).get_columns<std::string, std::string, std::string, bool>(0, 1, 2, 3);

            CurrencyInfo info;
            info.iso = iso;
            info.name = name;
            info.symbol = symbol;
            info.enabled = enabled;

            currencies.push_back(info);
        }

        int status = qry.finish();
        finishTableOperation(Currencies, Read, status);

        isGetCurrenciesDataFromDB = false;
    }

    return currencies;
}

CurrencyInfo DexDB::getCurrencyInfo(const std::string &iso)
{
    CurrencyInfo info;

    if (isGetCurrenciesDataFromDB) {
        std::string query = "SELECT name, symbol, enabled FROM currencies WHERE iso = '" + iso + "'";

        sqlite3pp::query qry(db, query.c_str());

        sqlite3pp::query::iterator i = qry.begin();
        std::string name;
        std::string symbol;
        bool enabled;
        std::tie(name, symbol, enabled) = (*i).get_columns<std::string, std::string, bool>(0, 1, 2);

        info.iso = iso;
        info.name = name;
        info.symbol = symbol;
        info.enabled = enabled;

        int status = qry.finish();
        finishTableOperation(Currencies, Read, status);
    } else {
        auto found = std::find_if(currencies.begin(), currencies.end(), [iso](CurrencyInfo info){ return info.iso == iso; });
        if (found != currencies.end()) {
            info = *found;
        }
    }

    return info;
}

void DexDB::addPaymentMethod(const unsigned char &type, const std::string &name, const std::string &description, const int &sortOrder)
{
    payments.push_back({type, name, description});

    sqlite3pp::command cmd(db, "INSERT INTO paymentMethods (type, name, description, sortOrder) VALUES (?, ?, ?, ?)");
    cmd.bind(1, type);
    cmd.bind(2, name, sqlite3pp::nocopy);
    cmd.bind(3, description, sqlite3pp::nocopy);
    cmd.bind(4, sortOrder);

    int status = cmd.execute();
    finishTableOperation(PaymentMethods, Add, status);
}

void DexDB::deletePaymentMethod(const unsigned char &type)
{
    payments.remove_if([type](PaymentMethodInfo c){return c.type == type;});

    sqlite3pp::command cmd(db, "DELETE FROM paymentMethods WHERE type = ?");
    cmd.bind(1, type);

    int status = cmd.execute();
    finishTableOperation(PaymentMethods, Delete, status);
}

std::list<PaymentMethodInfo> DexDB::getPaymentMethodsInfo()
{
    if (isGetPaymentsDataFromDB) {
        sqlite3pp::query qry(db, "SELECT type, name, description FROM paymentMethods ORDER BY sortOrder");

        for (sqlite3pp::query::iterator i = qry.begin(); i != qry.end(); ++i) {
            unsigned char type;
            std::string name;
            std::string description;
            std::tie(type, name, description) = (*i).get_columns<unsigned char, std::string, std::string>(0, 1, 2);

            PaymentMethodInfo info;
            info.type = type;
            info.name = name;
            info.description = description;
            payments.push_back(info);
        }

        int status = qry.finish();
        finishTableOperation(PaymentMethods, Read, status);

        isGetPaymentsDataFromDB = false;
    }

    return payments;
}

PaymentMethodInfo DexDB::getPaymentMethodInfo(const unsigned char &type)
{
    PaymentMethodInfo info;

    sqlite3pp::query qry(db, "SELECT name, description, description FROM paymentMethods WHERE type = ?");
    qry.bind(1, type);

    sqlite3pp::query::iterator i = qry.begin();
    std::string name;
    std::string description;
    std::tie(name, description) = (*i).get_columns<std::string, std::string>(0, 1);

    info.type = type;
    info.name = name;
    info.description = description;

    int status = qry.finish();
    finishTableOperation(PaymentMethods, Read, status);

    return info;
}

void DexDB::addOfferSell(const OfferInfo &offer)
{
    addOffer("offersSell", offer);
}

void DexDB::editOfferSell(const OfferInfo &offer)
{
    editOffer("offersSell", offer);
}

void DexDB::deleteOfferSell(const uint256 &idTransaction)
{
    deleteOffer("offersSell", idTransaction);
}

void DexDB::deleteOfferSellByHash(const uint256 &hash)
{
    deleteOfferByHash("offersSell", hash);
}

void DexDB::deleteOldOffersSell()
{
    deleteOldOffers("offersSell");
}

std::list<OfferInfo> DexDB::getOffersSell()
{
    return getOffers("offersSell");
}

std::list<OfferInfo> DexDB::getOffersSell(const std::string &countryIso, const std::string &currencyIso, const unsigned char &payment, const int &limit, const int &offset)
{
    return getOffers("offersSell", countryIso, currencyIso, payment, limit, offset);
}

OfferInfo DexDB::getOfferSell(const uint256 &idTransaction)
{
    return getOffer("offersSell", idTransaction);
}

OfferInfo DexDB::getOfferSellByHash(const uint256 &hash)
{
    return getOfferByHash("offersSell", hash);
}

bool DexDB::isExistOfferSell(const uint256 &idTransaction)
{
    return isExistOffer("offersSell", idTransaction);
}

bool DexDB::isExistOfferSellByHash(const uint256 &hash)
{
    return isExistOfferByHash("offersSell", hash);
}

std::list<uint256> DexDB::getSellHashs()
{
    return getHashs("offersSell");
}


size_t DexDB::countOffersSell()
{
    std::string tableName = "offersSell";

    int status;
    auto count = countOffers(tableName, status);
    finishTableOperation(OffersSell, Read, status);

    return count;
}

size_t DexDB::countOffersSell(const DexDB::OffersPeriod &from, const long long &timeMod)
{
    std::string tableName = "offersSell";

    int status;
    auto count = countOffers(tableName, from, timeMod, status);
    finishTableOperation(OffersSell, Read, status);

    return count;
}

size_t DexDB::countOffersSell(const std::string &countryIso, const std::string &currencyIso, const unsigned char &payment)
{
    std::string tableName = "offersSell";

    int status;
    auto count = countOffers(tableName, countryIso, currencyIso, payment, -1, 0, status);
    finishTableOperation(OffersSell, Read, status);

    return count;
}

uint64_t DexDB::lastModificationOffersSell()
{
    std::string tableName = "offersSell";

    int status;
    uint64_t time = lastModificationOffers(tableName, status);
    finishTableOperation(OffersBuy, Read, status);

    return time;
}

std::list<std::pair<uint256, uint32_t> > DexDB::getHashsAndEditingVersionsSell()
{
    return getHashsAndEditingVersions(TableName::offersSell, OffersPeriod::All, 0);
}

std::list<std::pair<uint256, uint32_t> > DexDB::getHashsAndEditingVersionsSell(const DexDB::OffersPeriod &from, const long long &timeMod)
{
    return getHashsAndEditingVersions(TableName::offersSell, from, timeMod);
}

void DexDB::addOfferBuy(const OfferInfo &offer)
{
    addOffer("offersBuy", offer);
}


void DexDB::editOfferBuy(const OfferInfo &offer)
{
    editOffer("offersBuy", offer);
}


void DexDB::deleteOfferBuy(const uint256 &idTransaction)
{
    deleteOffer("offersBuy", idTransaction);
}

void DexDB::deleteOfferBuyByHash(const uint256 &hash)
{
    deleteOfferByHash("offersBuy", hash);
}

void DexDB::deleteOldOffersBuy()
{
    deleteOldOffers("offersBuy");
}

std::list<OfferInfo> DexDB::getOffersBuy()
{
    return getOffers("offersBuy");
}

std::list<OfferInfo> DexDB::getOffersBuy(const std::string &countryIso, const std::string &currencyIso, const unsigned char &payment, const int &limit, const int &offset)
{
    return getOffers("offersBuy", countryIso, currencyIso, payment, limit, offset);
}

OfferInfo DexDB::getOfferBuy(const uint256 &idTransaction)
{
    return getOffer("offersBuy", idTransaction);
}

OfferInfo DexDB::getOfferBuyByHash(const uint256 &hash)
{
    return getOfferByHash("offersBuy", hash);
}

bool DexDB::isExistOfferBuy(const uint256 &idTransaction)
{
    return isExistOffer("offersBuy", idTransaction);
}

bool DexDB::isExistOfferBuyByHash(const uint256 &hash)
{
    return isExistOfferByHash("offersBuy", hash);
}

std::list<uint256> DexDB::getBuyHashs()
{
    return getHashs("offersBuy");
}


size_t DexDB::countOffersBuy()
{
    std::string tableName = "offersBuy";

    int status;
    auto count = countOffers(tableName, status);
    finishTableOperation(OffersBuy, Read, status);

    return count;
}

size_t DexDB::countOffersBuy(const DexDB::OffersPeriod &from, const long long &timeMod)
{
    std::string tableName = "offersBuy";

    int status;
    auto count = countOffers(tableName, from, timeMod, status);
    finishTableOperation(OffersBuy, Read, status);

    return count;
}

size_t DexDB::countOffersBuy(const std::string &countryIso, const std::string &currencyIso, const unsigned char &payment)
{
    std::string tableName = "offersBuy";

    int status;
    auto count = countOffers(tableName, countryIso, currencyIso, payment, -1, 0, status);
    finishTableOperation(OffersBuy, Read, status);

    return count;
}

uint64_t DexDB::lastModificationOffersBuy()
{
    std::string tableName = "offersBuy";

    int status;
    uint64_t time = lastModificationOffers(tableName, status);
    finishTableOperation(OffersBuy, Read, status);

    return time;
}

std::list<std::pair<uint256, uint32_t> > DexDB::getHashsAndEditingVersionsBuy()
{
    return getHashsAndEditingVersions(TableName::offersBuy, OffersPeriod::All, 0);
}

std::list<std::pair<uint256, uint32_t> > DexDB::getHashsAndEditingVersionsBuy(const OffersPeriod &from, const long long &timeMod)
{
    return getHashsAndEditingVersions(TableName::offersBuy, from, timeMod);
}

void DexDB::addMyOffer(const MyOfferInfo &offer)
{
    std::string query = "INSERT INTO myOffers (idTransaction, hash, pubKey, countryIso, currencyIso, "
                        "paymentMethod, price, minAmount, timeCreate, timeToExpiration, timeModification, shortInfo, details, type, status, editingVersion, editsign) "
                        "VALUES (:idTransaction, :hash, :pubKey, :countryIso, :currencyIso, "
                        ":paymentMethod, :price, :minAmount, :timeCreate, :timeToExpiration, :timeModification, :shortInfo, :details, :type, :status, :editingVersion, :editsign)";



    int status = addOrEditMyOffer(query, offer);
    finishTableOperation(MyOffers, Add, status);
}


void DexDB::editMyOffer(const MyOfferInfo &offer)
{
    std::string query = "UPDATE myOffers SET idTransaction = :idTransaction, countryIso = :countryIso, currencyIso = :currencyIso, "
                        "paymentMethod = :paymentMethod, price = :price, minAmount = :minAmount, "
                        "timeCreate = :timeCreate, timeToExpiration = :timeToExpiration, timeModification = :timeModification, "
                        "shortInfo = :shortInfo, details = :details, "
                        "type = :type, status = :status, editingVersion = :editingVersion, editsign = :editsign WHERE hash = :hash";


    int status = addOrEditMyOffer(query, offer);
    finishTableOperation(MyOffers, Edit, status);
}


void DexDB::deleteMyOffer(const uint256 &idTransaction)
{
    deleteOffer("myOffers", idTransaction);
}

void DexDB::deleteMyOfferByHash(const uint256 &hash)
{
    deleteOfferByHash("myOffers", hash);
}

void DexDB::deleteOldMyOffers()
{
    deleteOldOffers("myOffers");
}

bool DexDB::isExistMyOffer(const uint256 &idTransaction)
{
    int count = 0;

    std::string query = "SELECT count() FROM myOffers WHERE idTransaction = \"" + idTransaction.GetHex() + "\"";
    sqlite3pp::query qry(db, query.c_str());

    sqlite3pp::query::iterator it = qry.begin();
    (*it).getter() >> count;

    int status = qry.finish();
    finishTableOperation(MyOffers, Read, status);

    if (count > 0) {
        return true;
    }

    return false;
}

bool DexDB::isExistMyOfferByHash(const uint256 &hash)
{
    int count = 0;

    std::string query = "SELECT count() FROM myOffers WHERE hash = \"" + hash.GetHex() + "\"";
    sqlite3pp::query qry(db, query.c_str());

    sqlite3pp::query::iterator it = qry.begin();
    (*it).getter() >> count;

    int status = qry.finish();
    finishTableOperation(MyOffers, Read, status);

    if (count > 0) {
        return true;
    }

    return false;
}

std::list<MyOfferInfo> DexDB::getMyOffers()
{
    std::list<MyOfferInfo> offers;

    std::string str = "SELECT idTransaction, hash, pubKey, countryIso, currencyIso, paymentMethod, price, minAmount, timeCreate, "
                      "timeToExpiration, timeModification, shortInfo, details, type, status, editingVersion, editsign FROM myOffers";

    sqlite3pp::query qry(db, str.c_str());

    for (sqlite3pp::query::iterator i = qry.begin(); i != qry.end(); ++i) {
        MyOfferInfo info = getMyOffer(i);
        offers.push_back(info);
    }

    int status = qry.finish();
    finishTableOperation(MyOffers, Read, status);

    return offers;
}

std::list<MyOfferInfo> DexDB::getMyOffers(const std::string &countryIso, const std::string &currencyIso, const unsigned char &payment, const int &type, const int &statusOffer, const int &limit, const int &offset)
{
    std::list<MyOfferInfo> offers;

    std::string strQuery = "SELECT idTransaction, hash, pubKey, countryIso, currencyIso, paymentMethod, price, minAmount, timeCreate, "
                           "timeToExpiration, timeModification, shortInfo, details, type, status, editingVersion, editsign FROM myOffers";

    std::string where = "";
    if (countryIso != "") {
        where += " countryIso = '" + countryIso + "'";
    }

    if (currencyIso != "") {
        if (where != "") {
            where += " AND";
        }

        where += " currencyIso = '" + currencyIso + "'";
    }

    if (payment > 0) {
        if (where != "") {
            where += " AND";
        }

        where += " paymentMethod = " + std::to_string(payment);
    }

    if (type >= 0) {
        if (where != "") {
            where += " AND";
        }

        where += " type = " + std::to_string(type);
    }

    if (statusOffer > 0) {
        if (where != "") {
            where += " AND";
        }

        where += " status = " + std::to_string(statusOffer);
    }

    if (where != "") {
        strQuery += " WHERE" + where;
    }

    if (limit > 0) {
        strQuery += " LIMIT " + std::to_string(limit);

        if (offset > 0) {
            strQuery += " OFFSET " + std::to_string(offset);
        }
    }

    sqlite3pp::query qry(db, strQuery.c_str());

    for (sqlite3pp::query::iterator i = qry.begin(); i != qry.end(); ++i) {
        MyOfferInfo info = getMyOffer(i);
        offers.push_back(info);
    }

    int status = qry.finish();
    finishTableOperation(MyOffers, Read, status);

    return offers;
}

MyOfferInfo DexDB::getMyOffer(const uint256 &idTransaction)
{
    std::string str = "SELECT idTransaction, hash, pubKey, countryIso, currencyIso, paymentMethod, price, minAmount, timeCreate, "
                      "timeToExpiration, timeModification, shortInfo, details, type, status, editingVersion, editsign FROM myOffers WHERE idTransaction = \"" + idTransaction.GetHex() + "\"";

    sqlite3pp::query qry(db, str.c_str());

    sqlite3pp::query::iterator i = qry.begin();
    MyOfferInfo info = getMyOffer(i);

    int stat = qry.finish();
    finishTableOperation(MyOffers, Read, stat);

    return info;
}

MyOfferInfo DexDB::getMyOfferByHash(const uint256 &hash)
{
    std::string str = "SELECT idTransaction, hash, pubKey, countryIso, currencyIso, paymentMethod, price, minAmount, timeCreate, "
                      "timeToExpiration, timeModification, shortInfo, details, type, status, editingVersion, editsign FROM myOffers WHERE hash = \"" + hash.GetHex() + "\"";

    sqlite3pp::query qry(db, str.c_str());

    sqlite3pp::query::iterator i = qry.begin();
    MyOfferInfo info = getMyOffer(i);

    int stat = qry.finish();
    finishTableOperation(MyOffers, Read, stat);

    return info;
}

size_t DexDB::countMyOffers()
{
    std::string tableName = "myOffers";

    int status;
    auto count = countOffers(tableName, status);
    finishTableOperation(MyOffers, Read, status);

    return count;
}

size_t DexDB::countMyOffers(const std::string &countryIso, const std::string &currencyIso, const unsigned char &payment, const int &type, const int &statusOffer)
{
    std::string tableName = "myOffers";

    int status;
    auto count = countOffers(tableName, countryIso, currencyIso, payment, type, statusOffer, status);
    finishTableOperation(MyOffers, Read, status);

    return count;
}

MyOfferInfo DexDB::getMyOffer(sqlite3pp::query::iterator &it)
{
    std::string idTransaction;
    std::string hash;
    std::string pubKey;
    std::string countryIso;
    std::string currencyIso;
    uint8_t paymentMethod;
    long long int price;
    long long int minAmount;
    long long int timeCreate;
    long long int timeToExpiration;
    long long int timeModification;
    std::string shortInfo;
    std::string details;
    int type;
    int status;
    int editingVersion;
    std::string editsign;
    std::tie(idTransaction, hash, pubKey, countryIso, currencyIso, paymentMethod, price, minAmount,
             timeCreate, timeToExpiration, timeModification, shortInfo, details, type, status, editingVersion, editsign)
            = (*it).get_columns<std::string, std::string, std::string, std::string, std::string, uint8_t,
            long long int, long long int, long long int, long long int, long long int, std::string, std::string, int, int, int, std::string>
            (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);

    MyOfferInfo info;
    info.idTransaction.SetHex(idTransaction);
    info.hash.SetHex(hash);
    info.pubKey = pubKey;
    info.countryIso = countryIso;
    info.currencyIso = currencyIso;
    info.paymentMethod = paymentMethod;
    info.price = price;
    info.minAmount = minAmount;
    info.shortInfo = shortInfo;
    info.timeCreate = timeCreate;
    info.timeToExpiration = timeToExpiration;
    info.timeModification = timeModification;
    info.details = details;
    info.type = static_cast<TypeOffer>(type);
    info.status = static_cast<StatusOffer>(status);
    info.editingVersion = static_cast<uint32_t>(editingVersion);
    info.editsign = editsign;
    return info;
}


void DexDB::setStatusExpiredForMyOffers()
{
    std::string query = "UPDATE myOffers SET status = :status WHERE timeToExpiration < :currentTime";
    sqlite3pp::command cmd(db, query.c_str());

    uint64_t currentTime = static_cast<uint64_t>(time(NULL));
    cmd.bind(":currentTime", static_cast<long long int>(currentTime));
    cmd.bind(":status", StatusOffer::Expired);

    int status = cmd.execute();

    finishTableOperation(MyOffers, Edit, status);
}

void DexDB::editStatusForMyOffer(const uint256 &idTransaction, const StatusOffer &statusOffer)
{
    std::string query = "UPDATE myOffers SET status = :status WHERE idTransaction = :idTransaction";
    sqlite3pp::command cmd(db, query.c_str());

    cmd.bind(":idTransaction", idTransaction.GetHex(), sqlite3pp::copy);
    cmd.bind(":status", statusOffer);

    int status = cmd.execute();

    finishTableOperation(MyOffers, Edit, status);
}


void DexDB::addFilter(const std::string &filter)
{    
    sqlite3pp::command cmd(db, "INSERT INTO filterList (filter) VALUES (:filter)");
    cmd.bind(":filter", filter, sqlite3pp::nocopy);

    int status = cmd.execute();
    finishTableOperation(FiltersList, Add, status);
}

void DexDB::deleteFilter(const std::string &filter)
{    
    sqlite3pp::command cmd(db, "DELETE FROM filterList WHERE filter = :filter");
    cmd.bind(":filter", filter, sqlite3pp::nocopy);

    int status = cmd.execute();
    finishTableOperation(FiltersList, Delete, status);
}

std::list<std::string> DexDB::getFilters()
{
    std::list<std::string> filters;

    sqlite3pp::query qry(db, "SELECT filter FROM filterList");

    for (sqlite3pp::query::iterator i = qry.begin(); i != qry.end(); ++i) {
        std::string item;
        std::tie(item) = (*i).get_columns<std::string>(0);

        filters.push_back(item);
    }

    int status = qry.finish();
    finishTableOperation(FiltersList, Read, status);

    return filters;
}

void DexDB::addOffer(const std::string &tableName, const OfferInfo &offer)
{
    std::string query = "INSERT INTO " + tableName + " (idTransaction, hash, pubKey, countryIso, currencyIso, "
                        "paymentMethod, price, minAmount, timeCreate, timeToExpiration, timeModification, shortInfo, details, editingVersion, editsign) "
                        "VALUES (:idTransaction, :hash, :pubKey, :countryIso, :currencyIso, "
                        ":paymentMethod, :price, :minAmount, :timeCreate, :timeToExpiration, :timeModification, :shortInfo, :details, :editingVersion, :editsign)";

    int status = addOrEditOffer(query, offer);

    TypeTable tTable = OffersSell;
    if (tableName == "offersBuy") {
        tTable = OffersBuy;
    }

    finishTableOperation(tTable, Add, status);
}

void DexDB::editOffer(const std::string &tableName, const OfferInfo &offer)
{
    std::string query = "UPDATE " + tableName + " SET hash = :hash, pubKey = :pubKey, countryIso = :countryIso, currencyIso = :currencyIso, "
                                                "paymentMethod = :paymentMethod, price = :price, minAmount = :minAmount, "
                                                "timeCreate = :timeCreate, timeToExpiration = :timeToExpiration, timeModification = :timeModification, "
                                                "shortInfo = :shortInfo, details = :details, editingVersion = :editingVersion, editsign = :editsign WHERE hash = :hash";

    int status = addOrEditOffer(query, offer);

    TypeTable tTable = OffersSell;
    if (tableName == "offersBuy") {
        tTable = OffersBuy;
    }

    finishTableOperation(tTable, Edit, status);
}

int DexDB::addOrEditOffer(const std::string &query, const OfferInfo &offer)
{
    sqlite3pp::command cmd(db, query.c_str());

    bindOfferData(cmd, offer);

    return cmd.execute();
}

int DexDB::addOrEditMyOffer(const std::string &query, const MyOfferInfo &offer)
{
    sqlite3pp::command cmd(db, query.c_str());

    bindOfferData(cmd, offer.getOfferInfo());
    cmd.bind(":type", offer.type);
    cmd.bind(":status", offer.status);

    return cmd.execute();
}

void DexDB::bindOfferData(sqlite3pp::command &cmd, const OfferInfo &offer)
{
    std::string idTransaction = offer.idTransaction.GetHex();
    std::string hash = offer.hash.GetHex();

    cmd.bind(":idTransaction", idTransaction, sqlite3pp::copy);
    cmd.bind(":hash", hash, sqlite3pp::copy);
    cmd.bind(":pubKey", offer.pubKey, sqlite3pp::copy);
    cmd.bind(":countryIso", offer.countryIso, sqlite3pp::nocopy);
    cmd.bind(":currencyIso", offer.currencyIso, sqlite3pp::nocopy);
    cmd.bind(":paymentMethod", offer.paymentMethod);
    cmd.bind(":price", static_cast<long long int>(offer.price));
    cmd.bind(":minAmount", static_cast<long long int>(offer.minAmount));
    cmd.bind(":timeCreate", static_cast<long long int>(offer.timeCreate));
    cmd.bind(":timeToExpiration", static_cast<long long int>(offer.timeToExpiration));
    cmd.bind(":timeModification", static_cast<long long int>(offer.timeModification));
    cmd.bind(":shortInfo", offer.shortInfo, sqlite3pp::copy);
    cmd.bind(":details", offer.details, sqlite3pp::copy);
    cmd.bind(":editingVersion", static_cast<int>(offer.editingVersion));
    cmd.bind(":editsign", offer.editsign, sqlite3pp::copy);
}



void DexDB::deleteOffer(const std::string &tableName, const uint256 &idTransaction)
{
    std::string query = "DELETE FROM " + tableName + " WHERE idTransaction = ?";

    sqlite3pp::command cmd(db, query.c_str());
    cmd.bind(1, idTransaction.GetHex(), sqlite3pp::copy);

    int status = cmd.execute();
    TypeTable tTable = OffersSell;
    if (tableName == "offersBuy") {
        tTable = OffersBuy;
    } else if (tableName == "myOffers") {
        tTable = MyOffers;
    }

    finishTableOperation(tTable, Delete, status);
}

void DexDB::deleteOfferByHash(const std::string &tableName, const uint256 &hash)
{
    std::string query = "DELETE FROM " + tableName + " WHERE hash = ?";

    sqlite3pp::command cmd(db, query.c_str());
    cmd.bind(1, hash.GetHex(), sqlite3pp::copy);

    int status = cmd.execute();
    TypeTable tTable = OffersSell;
    if (tableName == "offersBuy") {
        tTable = OffersBuy;
    } else if (tableName == "myOffers") {
        tTable = MyOffers;
    }

    finishTableOperation(tTable, Delete, status);
}

void dex::DexDB::deleteOldOffers(const std::string &tableName)
{
    std::string query = "DELETE FROM " + tableName + " WHERE timeToExpiration <= :currentTime";

    long long int currentTime = static_cast<long long int>(time(NULL));
    sqlite3pp::command cmd(db, query.c_str());

    cmd.bind(":currentTime", currentTime);

    int status = cmd.execute();
    TypeTable tTable = OffersSell;
    if (tableName == "offersBuy") {
        tTable = OffersBuy;
    } else if (tableName == "myOffers") {
        tTable = MyOffers;
    }

    finishTableOperation(tTable, Delete, status);
}

std::list<OfferInfo> DexDB::getOffers(const std::string &tableName)
{
    std::list<OfferInfo> offers;

    std::string str = "SELECT idTransaction, hash, pubKey, countryIso, currencyIso, "
                      "paymentMethod, price, minAmount, timeCreate, timeToExpiration, timeModification, shortInfo, details, editingVersion, editsign FROM " + tableName;

    sqlite3pp::query qry(db, str.c_str());

    for (sqlite3pp::query::iterator i = qry.begin(); i != qry.end(); ++i) {
        OfferInfo info = getOffer(i);
        offers.push_back(info);
    }

    TypeTable tTable = OffersBuy;
    if (tableName == "offersSell") {
        tTable = OffersSell;
    }

    int status = qry.finish();
    finishTableOperation(tTable, Read, status);

    return offers;
}

std::list<OfferInfo> DexDB::getOffers(const std::string &tableName, const std::string &countryIso, const std::string &currencyIso, const unsigned char &payment, const int &limit, const int &offset)
{
    std::list<OfferInfo> offers;

    std::string strQuery = "SELECT idTransaction, hash, pubKey, countryIso, currencyIso, "
                      "paymentMethod, price, minAmount, timeCreate, timeToExpiration, timeModification, shortInfo, details, editingVersion, editsign FROM " + tableName;

    std::string where = "";
    if (countryIso != "") {
        where += " countryIso = '" + countryIso + "'";
    }

    if (currencyIso != "") {
        if (where != "") {
            where += " AND";
        }

        where += " currencyIso = '" + currencyIso + "'";
    }

    if (payment > 0) {
        if (where != "") {
            where += " AND";
        }

        where += " paymentMethod = " + std::to_string(payment);
    }

    if (where != "") {
        strQuery += " WHERE" + where;
    }

    if (limit > 0) {
        strQuery += " LIMIT " + std::to_string(limit);

        if (offset > 0) {
            strQuery += " OFFSET " + std::to_string(offset);
        }
    }

    sqlite3pp::query qry(db, strQuery.c_str());

    for (sqlite3pp::query::iterator i = qry.begin(); i != qry.end(); ++i) {
        OfferInfo info = getOffer(i);
        offers.push_back(info);
    }

    TypeTable tTable = OffersBuy;
    if (tableName == "offersSell") {
        tTable = OffersSell;
    }

    int status = qry.finish();
    finishTableOperation(tTable, Read, status);

    return offers;
}

OfferInfo DexDB::getOffer(const std::string &tableName, const uint256 &idTransaction)
{
    std::string str = "SELECT idTransaction, hash, pubKey, countryIso, currencyIso, "
                      "paymentMethod, price, minAmount, timeCreate, timeToExpiration, timeModification, shortInfo, details, editingVersion, editsign FROM " + tableName
                      + " WHERE idTransaction = \"" + idTransaction.GetHex() + "\"";

    int status;
    OfferInfo info = getOffer(str, status);

    TypeTable tTable = OffersBuy;
    if (tableName == "offersSell") {
        tTable = OffersSell;
    }

    finishTableOperation(tTable, Read, status);

    return info;
}

OfferInfo DexDB::getOfferByHash(const std::string &tableName, const uint256 &hash)
{
    std::string str = "SELECT idTransaction, hash, pubKey, countryIso, currencyIso, "
                      "paymentMethod, price, minAmount, timeCreate, timeToExpiration, timeModification, shortInfo, details, editingVersion, editsign FROM " + tableName
                      + " WHERE hash = \"" + hash.GetHex() + "\"";

    int status;
    OfferInfo info = getOffer(str, status);

    TypeTable tTable = OffersBuy;
    if (tableName == "offersSell") {
        tTable = OffersSell;
    }

    finishTableOperation(tTable, Read, status);

    return info;
}

OfferInfo DexDB::getOffer(const std::string &guery, int &status)
{
    sqlite3pp::query qry(db, guery.c_str());
    sqlite3pp::query::iterator i = qry.begin();
    OfferInfo info = getOffer(i);

    status = qry.finish();

    return info;
}

OfferInfo DexDB::getOffer(sqlite3pp::query::iterator &it)
{
    std::string idTransaction;
    std::string hash;
    std::string pubKey;
    std::string countryIso;
    std::string currencyIso;
    uint8_t paymentMethod;
    long long int price;
    long long int minAmount;
    long long int timeCreate;
    long long int timeToExpiration;
    long long int timeModification;
    std::string shortInfo;
    std::string details;
    int editingVersion;
    std::string editsign;
    std::tie(idTransaction, hash, pubKey, countryIso, currencyIso, paymentMethod, price, minAmount,
             timeCreate, timeToExpiration, timeModification, shortInfo, details, editingVersion, editsign)
            = (*it).get_columns<std::string, std::string, std::string, std::string, std::string, uint8_t,
            long long int, long long int, long long int, long long int, long long int, std::string, std::string, int, std::string>
            (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14);

    OfferInfo info;
    info.idTransaction.SetHex(idTransaction);
    info.hash.SetHex(hash);
    info.pubKey = pubKey;
    info.countryIso = countryIso;
    info.currencyIso = currencyIso;
    info.paymentMethod = paymentMethod;
    info.price = price;
    info.minAmount = minAmount;
    info.shortInfo = shortInfo;
    info.timeCreate = timeCreate;
    info.timeToExpiration = timeToExpiration;
    info.timeModification = timeModification;
    info.details = details;
    info.editingVersion = static_cast<uint32_t>(editingVersion);
    info.editsign = editsign;

    return info;
}

bool DexDB::isExistOffer(const std::string &tableName, const uint256 &idTransaction)
{
    int count = 0;

    std::string query = "SELECT count() FROM " + tableName + " WHERE idTransaction = \"" + idTransaction.GetHex() + "\"";
    sqlite3pp::query qry(db, query.c_str());

    sqlite3pp::query::iterator it = qry.begin();
    (*it).getter() >> count;

    TypeTable tTable = OffersBuy;
    if (tableName == "offersSell") {
        tTable = OffersSell;
    }

    int status = qry.finish();
    finishTableOperation(tTable, Read, status);

    if (count > 0) {
        return true;
    }

    return false;
}

bool DexDB::isExistOfferByHash(const std::string &tableName, const uint256 &hash)
{
    int count = 0;

    std::string query = "SELECT count() FROM " + tableName + " WHERE hash = \"" + hash.GetHex() + "\"";
    sqlite3pp::query qry(db, query.c_str());

    sqlite3pp::query::iterator it = qry.begin();
    (*it).getter() >> count;

    TypeTable tTable = OffersBuy;
    if (tableName == "offersSell") {
        tTable = OffersSell;
    }

    int status = qry.finish();
    finishTableOperation(tTable, Read, status);

    if (count > 0) {
        return true;
    }

    return false;
}

std::list<std::pair<uint256, uint32_t>> DexDB::getHashsAndEditingVersions(const std::string &tableName, const OffersPeriod &from, const long long &timeMod)
{
    std::list<std::pair<uint256, uint32_t>> vHashesAndEditingVersions;

    std::string str = "SELECT DISTINCT hash, editingVersion FROM " + tableName;

    if (OffersPeriod::Before == from) {
        str += " WHERE timeModification < :timeMod";
    } else if (OffersPeriod::After == from) {
        str += " WHERE timeModification >= :timeMod";
    }

    sqlite3pp::query qry(db, str.c_str());

    if (OffersPeriod::All != from) {
        qry.bind(":timeMod", timeMod);
    }

    for (sqlite3pp::query::iterator i = qry.begin(); i != qry.end(); ++i) {
        std::string sHash;
        int iEditingVersion;
        std::tie(sHash, iEditingVersion) = (*i).get_columns<std::string, int>(0, 1);
        uint256 uiHash;
        uiHash.SetHex(sHash);
        std::pair<uint256,int> pHashAndEditingVersion = std::make_pair(uiHash, iEditingVersion);
        vHashesAndEditingVersions.push_back(pHashAndEditingVersion);
    }

    TypeTable tTable = OffersBuy;
    if (tableName == "offersSell") {
        tTable = OffersSell;
    }

    int status = qry.finish();
    finishTableOperation(tTable, Read, status);

    return vHashesAndEditingVersions;
}


std::list<uint256> DexDB::getHashs(const std::string &tableName)
{
    std::list<uint256> ids;

    std::string query = "SELECT hash FROM " + tableName;
    sqlite3pp::query qry(db, query.c_str());

    for (sqlite3pp::query::iterator it = qry.begin(); it != qry.end(); ++it) {
        std::string strHash;
        (*it).getter() >> strHash;
        uint256 hash;
        hash.SetHex(strHash);

        ids.push_back(hash);
    }

    TypeTable tTable = OffersBuy;
    if (tableName == "offersSell") {
        tTable = OffersSell;
    }

    int status = qry.finish();
    finishTableOperation(tTable, Read, status);

    return ids;
}

size_t DexDB::countOffers(const std::string &tableName, int &status)
{
    long long int count;
    std::string query = "SELECT count(*) FROM " + tableName;
    sqlite3pp::query qry(db, query.c_str());

    sqlite3pp::query::iterator it = qry.begin();
    (*it).getter() >> count;
    status = qry.finish();

    return count;
}

size_t DexDB::countOffers(const std::string &tableName, const DexDB::OffersPeriod &from, const long long &timeMod, int &status)
{
    long long int count;
    std::string query = "SELECT count(*) FROM " + tableName;

    if (OffersPeriod::Before == from) {
        query += " WHERE timeModification < :timeMod";
    } else if (OffersPeriod::After == from) {
        query += " WHERE timeModification >= :timeMod";
    }

    sqlite3pp::query qry(db, query.c_str());
    qry.bind(":timeMod", timeMod);

    sqlite3pp::query::iterator it = qry.begin();
    (*it).getter() >> count;
    status = qry.finish();

    return count;
}

size_t DexDB::countOffers(const std::string &tableName, const std::string &countryIso, const std::string &currencyIso, const unsigned char &payment, const int &type, const int &statusOffer, int &status)
{
    long long int count;
    std::list<MyOfferInfo> offers;

    std::string strQuery = "SELECT count(*) FROM " + tableName;

    std::string where = "";
    if (countryIso != "") {
        where += " countryIso = '" + countryIso + "'";
    }

    if (currencyIso != "") {
        if (where != "") {
            where += " AND";
        }

        where += " currencyIso = '" + currencyIso + "'";
    }

    if (payment > 0) {
        if (where != "") {
            where += " AND";
        }

        where += " paymentMethod = " + std::to_string(payment);
    }

    if (type >= 0) {
        if (where != "") {
            where += " AND";
        }

        where += " type = " + std::to_string(type);
    }

    if (statusOffer > 0) {
        if (where != "") {
            where += " AND";
        }

        where += " status = " + std::to_string(statusOffer);
    }

    if (where != "") {
        strQuery += " WHERE" + where;
    }

    sqlite3pp::query qry(db, strQuery.c_str());
    sqlite3pp::query::iterator it = qry.begin();
    (*it).getter() >> count;
    status = qry.finish();

    return count;
}

uint64_t DexDB::lastModificationOffers(const std::string &tableName, int &status)
{
    long long int count;
    std::string query = "SELECT MAX(timeModification) FROM " + tableName;
    sqlite3pp::query qry(db, query.c_str());

    sqlite3pp::query::iterator it = qry.begin();
    (*it).getter() >> count;
    status = qry.finish();

    return count;
}

int DexDB::editCountry(const std::string &iso, const bool &enabled, const int &sortOrder)
{
    sqlite3pp::command cmd(db, "UPDATE countries SET enabled = :enabled, sortOrder = :sortOrder WHERE iso = :iso");
    cmd.bind(":enabled", enabled);
    cmd.bind(":sortOrder", sortOrder);
    cmd.bind(":iso", iso, sqlite3pp::nocopy);

    return cmd.execute();
}

int DexDB::editCurrency(const std::string &iso, const bool &enabled, const int &sortOrder)
{
    sqlite3pp::command cmd(db, "UPDATE currencies SET enabled = :enabled, sortOrder = :sortOrder WHERE iso = :iso");
    cmd.bind(":enabled", enabled);
    cmd.bind(":sortOrder", sortOrder);
    cmd.bind(":iso", iso, sqlite3pp::nocopy);

    return cmd.execute();
}

void DexDB::finishTableOperation(const dex::TypeTable &tables, const dex::TypeTableOperation &operation, const int &status)
{
    if (status != 0) {
        throw DexDBException(status);
    }

    if (callBack.size() > 0) {
        StatusTableOperation s = Ok;
        if (status == 1) {
            s = Error;
        }

        for (auto &item : callBack) {
            if (item.first != nullptr) {
                item.first->finishTableOperation(tables, operation, s);
            }
        }
    }
}

void DexDB::createTables(sqlite3pp::database &db)
{
    db.execute("CREATE TABLE IF NOT EXISTS dbversion (version BIG INT)");
    db.execute("CREATE TABLE IF NOT EXISTS countries (iso VARCHAR(2) NOT NULL PRIMARY KEY, name VARCHAR(100), enabled BOOLEAN, currencyId INT, sortOrder INT)");
    db.execute("CREATE TABLE IF NOT EXISTS currencies (id INTEGER PRIMARY KEY, iso VARCHAR(3) UNIQUE, name VARCHAR(100), "
               "symbol VARCHAR(10), enabled BOOLEAN, sortOrder INT)");
    db.execute("CREATE TABLE IF NOT EXISTS paymentMethods (type TINYINT NOT NULL PRIMARY KEY, name VARCHAR(100), description BLOB, sortOrder INT)");
    db.execute(templateOffersTable("offersSell").c_str());
    db.execute(templateOffersTable("offersBuy").c_str());
    db.execute("CREATE TABLE IF NOT EXISTS myOffers (hash TEXT NOT NULL PRIMARY KEY, "
               "idTransaction TEXT, pubKey TEXT, countryIso VARCHAR(2), "
               "currencyIso VARCHAR(3), paymentMethod TINYINT, price UNSIGNED BIG INT, "
               "minAmount UNSIGNED BIG INT, timeCreate UNSIGNED BIG INT, timeToExpiration UNSIGNED BIG INT, timeModification UNSIGNED BIG INT,"
               "shortInfo VARCHAR(140), details TEXT, type INT, status INT, editingVersion INT, editsign VARCHAR(150))");

    db.execute("CREATE TABLE IF NOT EXISTS filterList (filter VARCHAR(100) NOT NULL PRIMARY KEY)");

    bOffersRescan = true;
}


void DexDB::createIndexes(sqlite3pp::database &db)
{
    db.execute("CREATE INDEX IF NOT EXISTS idx_offersSell_timeexp ON offersSell(timeToExpiration)");
    db.execute("CREATE INDEX IF NOT EXISTS idx_offersBuy_timeexp ON offersBuy(timeToExpiration)");
    db.execute("CREATE INDEX IF NOT EXISTS idx_offersMy_timeexp ON myOffers(timeToExpiration)");
    db.execute("CREATE UNIQUE INDEX IF NOT EXISTS hash_editing_version_buy on offersBuy (hash, editingVersion)");
    db.execute("CREATE UNIQUE INDEX IF NOT EXISTS hash_editing_version_sell on offersSell (hash, editingVersion)");
    db.execute("CREATE INDEX IF NOT EXISTS idx_offersSell_timemod ON offersSell(timeModification)");
    db.execute("CREATE INDEX IF NOT EXISTS idx_offersBuy_timemod ON offersBuy(timeModification)");
}

void DexDB::addDefaultData()
{
    DefaultDataForDb def;

    countries.clear();
    currencies.clear();
    payments.clear();

    isGetCountriesDataFromDB = true;
    isGetCurrenciesDataFromDB = true;
    isGetPaymentsDataFromDB = true;

    int count = tableCount("dbversion");
    if (count <= 0) {
        addDbVersion(dex::uiDexDBversionInCode); 
    }

    count = tableCount("currencies");
    if (count <= 0) {
        std::list<DefaultCurrency> currencies = def.dataCurrencies();
        currencies.sort([](const DefaultCurrency &first, const DefaultCurrency &second) {return first.sortOrder < second.sortOrder;});

        for (auto item : currencies) {
            addCurrency(item.iso, item.name, item.symbol, item.enabled, item.sortOrder);
        }

        isGetCurrenciesDataFromDB = false;
    }

    count = tableCount("countries");
    if (count <= 0) {
        std::list<DefaultCountry> countries = def.dataCountries();

        countries.sort(DefaultCountry::cmp_name);
        countries.sort(DefaultCountry::cmp_sortorder);

        int order = 0;
        for (auto item : countries) {
            addCountry(item.iso, item.name, item.currency, true, order++);
        }

        isGetCountriesDataFromDB = false;
    }

    count = tableCount("paymentMethods");
    if (count <= 0) {
        std::list<DefaultPaymentMethod> methods = def.dataPaymentMethods();

        for (auto item : methods) {
            addPaymentMethod(item.type, item.name, item.description, item.sortOrder);
        }

        isGetPaymentsDataFromDB = false;
    }

//    count = tableCount("offersBuy");
//    if (count <= 0) {
//        createTestOffers();
//    }
}

void DexDB::addDbVersion(const int& uiDexDbVersion)
{
    sqlite3pp::command cmd(db, "INSERT INTO dbversion (version) VALUES (?)");
    cmd.bind(1, uiDexDbVersion);
    cmd.execute();
}

int DexDB::tableCount(const std::string &tableName)
{
    int count = 0;

    std::string query = "SELECT count() FROM ";
    query.append(tableName);
    sqlite3pp::query qry(db, query.c_str());

    sqlite3pp::query::iterator it = qry.begin();
    (*it).getter() >> count;

    return count;
}

std::string DexDB::templateOffersTable(const std::string &tableName) const
{
    std::string query = "CREATE TABLE IF NOT EXISTS " + tableName + " (idTransaction TEXT NOT NULL, "
                        "hash TEXT NOT NULL PRIMARY KEY, pubKey TEXT, countryIso VARCHAR(2), "
                        "currencyIso VARCHAR(3), paymentMethod TINYINT, price UNSIGNED BIG INT, "
                        "minAmount UNSIGNED BIG INT, timeCreate UNSIGNED BIG INT, timeToExpiration UNSIGNED BIG INT, "
                        "timeModification UNSIGNED BIG INT, shortInfo VARCHAR(140), details TEXT, editingVersion UNSIGNED INT, "
                        "editsign VARCHAR(150))";
    return query;
}

int DexDB::backup(sqlite3pp::database &destdb)
{
    return db.backup(destdb);
}


int DexDB::vacuum()
{
    return db.execute("VACUUM");
}

int DexDB::begin()
{
    return db.execute("BEGIN");
}


int DexDB::commit()
{
    return db.execute("COMMIT");
}


int DexDB::rollback()
{
    return db.execute("ROLLBACK");
}


bool DexDB::AutoBackup (DexDB *db, int nBackups, std::string& strBackupWarning, std::string& strBackupError)
{
    namespace fs = boost::filesystem;

    strBackupWarning = strBackupError = "";

    if(nBackups > 0)
    {
        fs::path backupsDir = GetBackupsDir();
        fs::path dexdbpath = strDexDbFile;

        if (!fs::exists(backupsDir))
        {
            strBackupError = strprintf(_("Backup folder %s not found!"), backupsDir.string());
            return false;
        }

        std::string dateTimeStr = DateTimeStrFormat(".%Y-%m-%d-%H-%M", GetTime());
        if (db)
        {
            fs::path backupFile = backupsDir / (dexdbpath.filename().string() + dateTimeStr);
            sqlite3pp::database destdb(backupFile.string().c_str());
            if (db->backup(destdb) != SQLITE_DONE) {
                strBackupWarning = strprintf(_("Failed to create backup %s!"), backupFile.string().c_str());
                LogPrintf("%s\n", strBackupWarning);
                return false;
            }
        } else {
            fs::path sourceFile = dexdbpath;
            fs::path backupFile = backupsDir / (dexdbpath.filename().string() + dateTimeStr);
            sourceFile.make_preferred();
            backupFile.make_preferred();
            if (fs::exists(backupFile))
            {
                strBackupWarning = _("Failed to create backup, file already exists! This could happen if you restarted in less than 60 seconds. You can continue if you are ok with this.");
                LogPrintf("%s\n", strBackupWarning);
                return false;
            }
            if(fs::exists(sourceFile)) {
                try {
                    fs::copy_file(sourceFile, backupFile);
                    LogPrintf("Creating backup of %s -> %s\n", sourceFile.string(), backupFile.string());
                } catch(fs::filesystem_error &error) {
                    strBackupWarning = strprintf(_("Failed to create backup, error: %s"), error.what());
                    LogPrintf("%s\n", strBackupWarning);
                    return false;
                }
            }
        }

        // Keep only the last 10 backups, including the new one of course
        typedef std::multimap<std::time_t, fs::path> folder_set_t;
        folder_set_t folder_set;
        fs::directory_iterator end_iter;
        backupsDir.make_preferred();
        // Build map of backup files for current(!) wallet sorted by last write time
        fs::path currentFile;
        for (fs::directory_iterator dir_iter(backupsDir); dir_iter != end_iter; ++dir_iter)
        {
            // Only check regular files
            if ( fs::is_regular_file(dir_iter->status()))
            {
                currentFile = dir_iter->path().filename();
                // Only add the backups for the current dexdb, e.g. dexdb.dat.*
                if(dir_iter->path().stem().string() == dexdbpath.filename())
                {
                    folder_set.insert(folder_set_t::value_type(fs::last_write_time(dir_iter->path()), *dir_iter));
                }
            }
        }

        // Loop backward through backup files and keep the N newest ones (1 <= N <= 10)
        int counter = 0;
        for (auto rit = folder_set.crbegin(); rit != folder_set.crend(); ++rit)
        {
            counter++;
            if (counter > nBackups)
            {
                try {
                    fs::remove(rit->second);
                    LogPrintf("Old backup deleted: %s\n", rit->second);
                } catch(fs::filesystem_error &error) {
                    strBackupWarning = strprintf(_("Failed to delete backup, error: %s"), error.what());
                    LogPrintf("%s\n", strBackupWarning);
                    return false;
                }
            }
        }
        return true;
    }

    LogPrintf("Automatic dex DB backups are disabled!\n");
    return false;
}



}
