#pragma once

#include "av_cef_javascript.h"

#include <aardvark/aardvark_client.h>
#include <aardvark/aardvark_scene_graph.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <set>

using aardvark::AvSceneContext;
using aardvark::EAvSceneGraphResult;
using aardvark::EAvSceneGraphNodeType;

class DynamicFunctionHandler : public CefV8Handler 
{
public:
	DynamicFunctionHandler( const std::string & sFunctionName, JavascriptFn fn )
	{
		m_functionName = sFunctionName;
		m_fn = fn;
	}

	virtual bool Execute( const CefString& name,
		CefRefPtr<CefV8Value> object,
		const CefV8ValueList& arguments,
		CefRefPtr<CefV8Value>& retval,
		CefString& exception ) override 
	{
		if ( name == m_functionName && m_fn ) 
		{
			m_fn( arguments, retval, exception );
			return true;
		}

		// Function does not exist.
		return false;
	}

private:
	std::string m_functionName;
	JavascriptFn m_fn = nullptr;

	// Provide the reference counting implementation for this class.
	IMPLEMENT_REFCOUNTING( DynamicFunctionHandler );
};

class CJavascriptObjectWithFunctions
{
public:
	CJavascriptObjectWithFunctions();
	~CJavascriptObjectWithFunctions();

	virtual bool init() = 0;
	virtual void cleanup() = 0;

	CefRefPtr<CefV8Value> getContainer() { return m_container; }

protected:
	void RegisterFunction( const std::string & sName, JavascriptFn fn );

	CefRefPtr<CefV8Value> m_container;

};

CJavascriptObjectWithFunctions::CJavascriptObjectWithFunctions()
{
	m_container = CefV8Value::CreateObject( nullptr, nullptr );
}

CJavascriptObjectWithFunctions::~CJavascriptObjectWithFunctions()
{
	m_container = nullptr;
}

void CJavascriptObjectWithFunctions::RegisterFunction( const std::string & sName, JavascriptFn fn )
{
	CefRefPtr< DynamicFunctionHandler > pFunction( new DynamicFunctionHandler( sName, fn ) );
	m_container->SetValue( sName, CefV8Value::CreateFunction( sName, pFunction ), V8_PROPERTY_ATTRIBUTE_READONLY );
}

class CAardvarkAppObject : public CJavascriptObjectWithFunctions
{
	friend class CSceneContextObject;
public:
	CAardvarkAppObject( CAardvarkRenderProcessHandler *pRenderProcessHandler, AvApp::Client client, const std::string & name );

	virtual bool init() override;
	virtual void cleanup() override;

	void finishSceneContext( CSceneContextObject *contextObject );
	void updateNodeIdsForThisTexture( const std::vector<uint32_t> vecNewNodeIds );
	const std::string & getName() const { return m_name; }
private:
	AvApp::Client m_appClient;
	CAardvarkRenderProcessHandler *m_handler = nullptr;
	std::string m_name;
	std::list<std::unique_ptr<CSceneContextObject>> m_sceneContexts;
	std::set<uint32_t> m_nodeIdsThatNeedThisTexture;
};


class CSceneContextObject : public CJavascriptObjectWithFunctions
{
public:
	CSceneContextObject( CAardvarkAppObject *parentApp, CAardvarkRenderProcessHandler *pRenderProcessHandler, aardvark::AvSceneContext context );

	virtual bool init() override;
	virtual void cleanup() override;
	AvSceneContext getContext() { return m_context; }

	uint32_t getCurrentNodeId();
	std::vector<uint32_t> getNodeIdsThatWillNeedThisTexture() { return m_nodeIdsThatWillNeedThisTexture;  }
private:
	aardvark::AvSceneContext m_context;
	CAardvarkRenderProcessHandler *m_handler = nullptr;
	CAardvarkAppObject *m_parentApp = nullptr;
	std::vector<uint32_t> m_nodeIdsThatWillNeedThisTexture;
	std::vector<uint32_t> m_nodeIdStack;
};

CSceneContextObject::CSceneContextObject( CAardvarkAppObject *parentApp, CAardvarkRenderProcessHandler *renderProcessHandler, aardvark::AvSceneContext context )
{
	m_handler = renderProcessHandler;
	m_context = context;
	m_parentApp = parentApp;
}


uint32_t CSceneContextObject::getCurrentNodeId()
{
	if ( m_nodeIdStack.empty() )
	{
		return 0;
	}
	else
	{
		return m_nodeIdStack.back();
	}
}


bool CSceneContextObject::init()
{
	RegisterFunction( "finish", [this, parentApp = m_parentApp]( const CefV8ValueList & arguments, CefRefPtr<CefV8Value>& retval, CefString& exception )
	{
		if ( arguments.size() != 0 )
		{
			exception = "Invalid arguments";
			return;
		}

		parentApp->finishSceneContext( this );
	} );

	RegisterFunction( "startNode", [this]( const CefV8ValueList & arguments, CefRefPtr<CefV8Value>& retval, CefString& exception )
	{
		if ( arguments.size() != 3 )
		{
			exception = "Invalid argument count";
			return;
		}
		if ( !arguments[0]->IsUInt() || !( arguments[1]->IsString() || arguments[1]->IsNull() ) || !arguments[2]->IsInt() )
		{
			exception = "Invalid arguments";
			return;
		}

		std::string name;
		if ( arguments[1]->IsString() )
		{
			name = arguments[1]->GetStringValue();
		}

		EAvSceneGraphNodeType eType = (EAvSceneGraphNodeType)arguments[2]->GetIntValue();

		uint32_t nodeId = arguments[0]->GetUIntValue();
		EAvSceneGraphResult res = aardvark::avStartNode( m_context, nodeId, name.empty() ? nullptr : name.c_str(), eType );
		if ( res != EAvSceneGraphResult::Success )
		{
			exception = "avStartNode failed " + std::to_string( (int)res );
		}
		else
		{
			m_nodeIdStack.push_back( nodeId );
		}
	} );

	CefRefPtr<CefV8Value> typeEnum = CefV8Value::CreateObject(nullptr, nullptr);
	typeEnum->SetValue( "Container",	CefV8Value::CreateInt( (int32_t)EAvSceneGraphNodeType::Container ),	V8_PROPERTY_ATTRIBUTE_READONLY );
	typeEnum->SetValue( "Origin",		CefV8Value::CreateInt( (int32_t)EAvSceneGraphNodeType::Origin ),		V8_PROPERTY_ATTRIBUTE_READONLY );
	typeEnum->SetValue( "Transform",	CefV8Value::CreateInt( (int32_t)EAvSceneGraphNodeType::Transform ),	V8_PROPERTY_ATTRIBUTE_READONLY );
	typeEnum->SetValue( "Model",		CefV8Value::CreateInt( (int32_t)EAvSceneGraphNodeType::Model ),		V8_PROPERTY_ATTRIBUTE_READONLY );
	typeEnum->SetValue( "Panel",		CefV8Value::CreateInt( (int32_t)EAvSceneGraphNodeType::Panel ), V8_PROPERTY_ATTRIBUTE_READONLY );
	typeEnum->SetValue( "Poker",		CefV8Value::CreateInt( (int32_t)EAvSceneGraphNodeType::Poker ), V8_PROPERTY_ATTRIBUTE_READONLY );
	m_container->SetValue( "type", typeEnum, V8_PROPERTY_ATTRIBUTE_READONLY );

	RegisterFunction( "finishNode", [this]( const CefV8ValueList & arguments, CefRefPtr<CefV8Value>& retval, CefString& exception )
	{
		if ( arguments.size() != 0 )
		{
			exception = "Invalid argument count";
			return;
		}

		EAvSceneGraphResult res = aardvark::avFinishNode( m_context );
		if ( res != EAvSceneGraphResult::Success )
		{
			exception = "avFinishNode failed";
		}
		else
		{
			if ( !m_nodeIdStack.empty() )
			{
				m_nodeIdStack.pop_back();
			}
		}
	} );

	RegisterFunction( "setOriginPath", [this]( const CefV8ValueList & arguments, CefRefPtr<CefV8Value>& retval, CefString& exception )
	{
		if ( arguments.size() != 1 )
		{
			exception = "Invalid argument count";
			return;
		}
		if ( !arguments[0]->IsString() )
		{
			exception = "Invalid arguments";
			return;
		}

		EAvSceneGraphResult res = aardvark::avSetOriginPath( m_context, std::string( arguments[0]->GetStringValue() ).c_str() );
		if ( res != EAvSceneGraphResult::Success )
		{
			exception = "avSetOriginPath failed " + std::to_string( (int)res );
		}
	} );

	RegisterFunction( "setTranslation", [this]( const CefV8ValueList & arguments, CefRefPtr<CefV8Value>& retval, CefString& exception )
	{
		if ( arguments.size() != 3 )
		{
			exception = "Invalid argument count";
			return;
		}
		if ( !arguments[0]->IsDouble() || !arguments[1]->IsDouble() || !arguments[2]->IsDouble() )
		{
			exception = "Invalid arguments";
			return;
		}

		EAvSceneGraphResult res = aardvark::avSetTranslation( m_context, 
			(float)arguments[0]->GetDoubleValue(),
			(float)arguments[1]->GetDoubleValue(),
			(float)arguments[2]->GetDoubleValue() );
		if ( res != EAvSceneGraphResult::Success )
		{
			exception = "avSetTranslation failed " + std::to_string( (int)res );
		}
	} );

	RegisterFunction( "setScale", [this]( const CefV8ValueList & arguments, CefRefPtr<CefV8Value>& retval, CefString& exception )
	{
		if ( arguments.size() != 3 )
		{
			exception = "Invalid argument count";
			return;
		}
		if ( !arguments[0]->IsDouble() || !arguments[1]->IsDouble() || !arguments[2]->IsDouble() )
		{
			exception = "Invalid arguments";
			return;
		}

		EAvSceneGraphResult res = aardvark::avSetScale( m_context,
			(float)arguments[0]->GetDoubleValue(),
			(float)arguments[1]->GetDoubleValue(),
			(float)arguments[2]->GetDoubleValue() );
		if ( res != EAvSceneGraphResult::Success )
		{
			exception = "avSetScale failed " + std::to_string( (int)res );
		}
	} );

	RegisterFunction( "setUniformScale", [this]( const CefV8ValueList & arguments, CefRefPtr<CefV8Value>& retval, CefString& exception )
	{
		if ( arguments.size() != 1 )
		{
			exception = "Invalid argument count";
			return;
		}
		if ( !arguments[0]->IsDouble() )
		{
			exception = "Invalid arguments";
			return;
		}

		EAvSceneGraphResult res = aardvark::avSetScale( m_context,
			(float)arguments[0]->GetDoubleValue(),
			(float)arguments[0]->GetDoubleValue(),
			(float)arguments[0]->GetDoubleValue() );
		if ( res != EAvSceneGraphResult::Success )
		{
			exception = "avSetScale failed " + std::to_string( (int)res );
		}
	} );

	RegisterFunction( "setRotationEulerDegrees", [this]( const CefV8ValueList & arguments, CefRefPtr<CefV8Value>& retval, CefString& exception )
	{
		if ( arguments.size() != 3 )
		{
			exception = "Invalid argument count";
			return;
		}
		if ( !arguments[0]->IsDouble() || !arguments[1]->IsDouble() || !arguments[2]->IsDouble() )
		{
			exception = "Invalid arguments";
			return;
		}

		double fYawRadians = ( arguments[0]->GetDoubleValue() * M_PI / 180.0 );
		double fPitchRadians =( arguments[1]->GetDoubleValue() * M_PI / 180.0 );
		double fRollRadians = ( arguments[2]->GetDoubleValue() * M_PI / 180.0 );

		glm::quat rot = glm::quat( glm::vec3( fPitchRadians, fYawRadians, fRollRadians ) );

		EAvSceneGraphResult res = aardvark::avSetRotation( m_context,
			(float)rot.x,
			(float)rot.y,
			(float)rot.z,
			(float)rot.w );
		if ( res != EAvSceneGraphResult::Success )
		{
			exception = "avSetRotation failed " + std::to_string( (int)res );
		}
	} );

	RegisterFunction( "setModelUri", [this]( const CefV8ValueList & arguments, CefRefPtr<CefV8Value>& retval, CefString& exception )
	{
		if ( arguments.size() != 1 )
		{
			exception = "Invalid argument count";
			return;
		}
		if ( !arguments[0]->IsString() )
		{
			exception = "Invalid arguments";
			return;
		}

		EAvSceneGraphResult res = aardvark::avSetModelUri( m_context, std::string( arguments[0]->GetStringValue() ).c_str() );
		if ( res != EAvSceneGraphResult::Success )
		{
			exception = "avSetModelUri failed " + std::to_string( (int)res );
		}
	} );

	RegisterFunction( "setTextureSource", [this, handler = m_handler ]( const CefV8ValueList & arguments, CefRefPtr<CefV8Value>& retval, CefString& exception )
	{
		if ( arguments.size() != 1 )
		{
			exception = "Invalid argument count";
			return;
		}
		if ( !arguments[0]->IsString() )
		{
			exception = "Invalid arguments";
			return;
		}

		EAvSceneGraphResult res = aardvark::avSetPanelTextureSource( m_context, std::string( arguments[0]->GetStringValue() ).c_str() );
		if ( res != EAvSceneGraphResult::Success )
		{
			exception = "avSetPanelTextureSource failed " + std::to_string( (int)res );
		}
	} );

	RegisterFunction( "setInteractive", [this, handler = m_handler ]( const CefV8ValueList & arguments, CefRefPtr<CefV8Value>& retval, CefString& exception )
	{
		if ( arguments.size() != 1 )
		{
			exception = "Invalid argument count";
			return;
		}
		if ( !arguments[0]->IsBool() )
		{
			exception = "Invalid arguments";
			return;
		}

		EAvSceneGraphResult res = aardvark::avSetPanelInteractive( m_context, arguments[0]->GetBoolValue() );
		if ( res != EAvSceneGraphResult::Success )
		{
			exception = "avSetPanelInteractive failed " + std::to_string( (int)res );
		}
	} );

	return true;
}


void CSceneContextObject::cleanup()
{

}


CAardvarkAppObject::CAardvarkAppObject( CAardvarkRenderProcessHandler *renderProcessHandler, AvApp::Client client, const std::string & name )
	: m_appClient( client )
{
	m_handler = renderProcessHandler;
	m_name = name;
}


bool CAardvarkAppObject::init()
{
	RegisterFunction( "getName", [this]( const CefV8ValueList & arguments, CefRefPtr<CefV8Value>& retval, CefString& exception )
	{
		if ( arguments.size() != 0 )
		{
			exception = "Invalid arguments";
			return;
		}

		auto resName= m_appClient.nameRequest().send().wait( m_handler->getClient()->WaitScope() );
		if( resName.hasName() )
		{
			retval = CefV8Value::CreateString( resName.getName().cStr() );
		}
		else
		{
			retval = CefV8Value::CreateNull();
		}
	} );

	RegisterFunction( "startSceneContext", [this]( const CefV8ValueList & arguments, CefRefPtr<CefV8Value>& retval, CefString& exception )
	{
		if ( arguments.size() != 0 )
		{
			exception = "Invalid arguments";
			return;
		}

		AvSceneContext context;
		if ( aardvark::EAvSceneGraphResult::Success != aardvark::avStartSceneContext( m_handler->getClient(), &context ) )
		{
			exception = "Failed to start context";
			return;
		}

		auto newContext = std::make_unique<CSceneContextObject>( this, m_handler, context );
		if ( !newContext->init() )
		{
			exception = "Failed to init context";
			return;
		}
		retval = newContext->getContainer();

		m_sceneContexts.push_back( std::move( newContext ) );

	} );

	return true;
}

void CAardvarkAppObject::cleanup()
{
	for ( auto &context : m_sceneContexts )
	{
		context->cleanup();
	}
	m_sceneContexts.clear();

	m_appClient = nullptr;
}


void CAardvarkAppObject::finishSceneContext( CSceneContextObject *contextObject )
{
	avFinishSceneContext( contextObject->getContext(), &m_appClient );

	for ( auto iEntry = m_sceneContexts.begin(); iEntry != m_sceneContexts.end(); iEntry++ )
	{
		if ( &*(*iEntry) == contextObject )
		{
			m_sceneContexts.erase( iEntry );
			break;
		}
	}
}



class CAardvarkObject : public CJavascriptObjectWithFunctions
{
public:
	CAardvarkObject( CAardvarkRenderProcessHandler *pRenderProcessHandler );

	virtual bool init() override;
	void cleanup() override;
	std::list<std::unique_ptr<CAardvarkAppObject>> & getApps() { return m_apps;  }

	bool hasPermission( const std::string & permission );

private:
	CAardvarkRenderProcessHandler *m_handler = nullptr;
	std::list<std::unique_ptr<CAardvarkAppObject>> m_apps;
};

CAardvarkObject::CAardvarkObject( CAardvarkRenderProcessHandler *renderProcessHandler )
{
	m_handler = renderProcessHandler;
}

bool CAardvarkObject::hasPermission( const std::string & permission )
{
	return m_handler->hasPermission( permission );
}


bool CAardvarkObject::init()
{
	if ( hasPermission( "scenegraph" ) )
	{
		RegisterFunction( "createApp", [this]( const CefV8ValueList & arguments, CefRefPtr<CefV8Value>& retval, CefString& exception )
		{
			if ( arguments.size() != 1 )
			{
				exception = "Invalid arguments";
				return;
			}
			if ( !arguments[0]->IsString() )
			{
				exception = "Invalid name argument";
				return;
			}

			auto reqCreateExampleApp = m_handler->getClient()->Server().createAppRequest();
			reqCreateExampleApp.setName( std::string( arguments[0]->GetStringValue() ).c_str() );
			auto resCreateApp = reqCreateExampleApp.send().wait( m_handler->getClient()->WaitScope() );
			if ( !resCreateApp.hasApp() )
			{
				retval = CefV8Value::CreateNull();
			}
			else
			{
				auto app = std::make_unique<CAardvarkAppObject>( m_handler, resCreateApp.getApp(), std::string( arguments[0]->GetStringValue() ) );
				if ( !app->init() )
				{
					retval = CefV8Value::CreateNull();
				}
				else
				{
					retval = app->getContainer();
					m_apps.push_back( std::move( app ) );
					m_handler->updateAppNamesForBrowser();
				}
			}
		} );
	}

	if ( hasPermission( "master" ) )
	{
		RegisterFunction( "startApp", [this]( const CefV8ValueList & arguments, CefRefPtr<CefV8Value>& retval, CefString& exception )
		{
			if ( arguments.size() != 2 )
			{
				exception = "Invalid arguments";
				return;
			}
			if ( !arguments[0]->IsString() )
			{
				exception = "Invalid url argument";
				return;
			}
			if ( !arguments[1]->IsArray() )
			{
				exception = "Invalid permission argument";
				return;
			}

			size_t listIndex = 0;
			CefRefPtr< CefListValue> permissionList = CefListValue::Create();
			for ( size_t n = 0; n < arguments[1]->GetArrayLength(); n++ )
			{
				auto arrayValue = arguments[1]->GetValue( (int)n );
				if ( arrayValue->IsString() )
				{
					permissionList->SetString( listIndex++, arrayValue->GetStringValue() );
				}
			}

			CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create( "start_app" );

			msg->GetArgumentList()->SetString( 0, arguments[0]->GetStringValue() );
			msg->GetArgumentList()->SetList( 1, permissionList );
			m_handler->getBrowser()->SendProcessMessage( PID_BROWSER, msg );
		} );
	}

	return true;

}

void CAardvarkObject::cleanup()
{
	for ( auto & app : m_apps )
	{
		app->cleanup();
	}
	m_apps.clear();
}


CAardvarkRenderProcessHandler::CAardvarkRenderProcessHandler()
{
	m_client = std::make_unique< aardvark::CAardvarkClient >();
}


void CAardvarkRenderProcessHandler::OnContextCreated(
	CefRefPtr<CefBrowser> browser,
	CefRefPtr<CefFrame> frame,
	CefRefPtr<CefV8Context> context )
{
	assert( !m_browser );
	m_browser = browser;
	m_context = context;
	m_client->Start();

	// Retrieve the context's window object.
	CefRefPtr<CefV8Value> windowObj = m_context->GetGlobal();

	// Create an object to store our functions in
	m_aardvarkObject = std::make_unique<CAardvarkObject>( this );
	assert( m_aardvarkObject->init() );
	windowObj->SetValue( "aardvark", m_aardvarkObject->getContainer(), V8_PROPERTY_ATTRIBUTE_READONLY );

}

void CAardvarkRenderProcessHandler::OnContextReleased( CefRefPtr<CefBrowser> browser,
	CefRefPtr<CefFrame> frame,
	CefRefPtr<CefV8Context> context )
{
	m_aardvarkObject->cleanup();
	m_aardvarkObject = nullptr;
	m_client->Stop();
	m_browser = nullptr;
}


bool CAardvarkRenderProcessHandler::OnProcessMessageReceived( CefRefPtr<CefBrowser> browser,
	CefProcessId source_process,
	CefRefPtr<CefProcessMessage> message )
{
	std::string messageName = message->GetName();

	if ( messageName == "set_browser_permissions" )
	{
		m_permissions.clear();
		auto permList = message->GetArgumentList()->GetList( 0 );
		for ( size_t n = 0; n < permList->GetSize(); n++ )
		{
			std::string perm( permList->GetString( n ) );
			m_permissions.insert( perm );
		}
	}
	return false;
}


void CAardvarkRenderProcessHandler::updateAppNamesForBrowser()
{
	size_t listIndex = 0;
	CefRefPtr< CefListValue> nameList = CefListValue::Create();
	for ( auto & app : m_aardvarkObject->getApps() )
	{
		nameList->SetString( listIndex++, app->getName() );
	}

	CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create( "update_browser_app_names" );

	msg->GetArgumentList()->SetList( 0, nameList );
	m_browser->SendProcessMessage( PID_BROWSER, msg );
}

bool CAardvarkRenderProcessHandler::hasPermission( const std::string & permission )
{
	return m_permissions.find( permission ) != m_permissions.end();
}
